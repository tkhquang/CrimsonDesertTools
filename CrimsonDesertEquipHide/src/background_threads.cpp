#include "background_threads.hpp"
#include "constants.hpp"
#include "indexed_string_table.hpp"
#include "player_detection.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit/detail/worker.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace EquipHide
{
    namespace
    {
        // s_workers_mtx serializes the lazy-launch arms. launch_*() sets each worker pointer under the lock and
        // join_background_threads() reads it unlocked. The atomic launch guards stop a second launch_*() call from a
        // race against the first, so the unique_ptr writes see no contention beyond the launch site.
        std::mutex s_workers_mtx;
        std::unique_ptr<DetourModKit::StoppableWorker> s_deferred_scan_worker;
        std::unique_ptr<DetourModKit::StoppableWorker> s_lazy_probe_worker;
        std::unique_ptr<DetourModKit::StoppableWorker> s_resolve_poll_worker;

        // Deferred IndexedStringA scan tuning. Convergence rests on two gates plus a stability window.
        //
        // World-ready: user+0xD8 must hold a non-null controlled actor. In main-menu or loading-screen state the
        // IndexedStringA table carries a tiny engine-internal seed of about three entries that stays "stable" for
        // many seconds and false-triggers a pure stability check. A walk of the same chain resolve_poll_body already
        // polls lets the gate refuse the commit until the world is live.
        //
        // Minimum count: even once the chain reports a controlled actor, the gate refuses a commit below
        // MIN_STABLE_COUNT, so a torn or partially loaded registry cannot snapshot a sub-50 entry table that happens
        // to repeat itself across consecutive polls.
        //
        // Stability window: with both gates satisfied, STABILITY_REQUIRED identical scans confirm that the table
        // finished growing for this load.
        constexpr int SCAN_RETRY_MS = 2000;
        constexpr int SCAN_INITIAL_DELAY_MS = 8000;
        constexpr int SCAN_HEARTBEAT_EVERY = 30;
        constexpr int STABILITY_REQUIRED = 5;
        constexpr std::size_t MIN_STABLE_COUNT = 32;

        // Forward declaration. The world-ready probe lives below, next to the resolve-poll thread that consumes it,
        // and the deferred scan body calls it as a commit gate.
        std::uintptr_t read_controlled_actor_ptr() noexcept;

        // Sleeps in short slices and observes both the StoppableWorker stop_token and the process-wide
        // shutdown_requested() flag, so the worker exits within about 50ms of either signal whatever the
        // per-iteration retry delay is.
        bool sleep_responsive_ms(std::stop_token st, int total_ms) noexcept
        {
            constexpr int slice_ms = 50;
            int remaining = total_ms;
            while (remaining > 0)
            {
                if (st.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                    return false;
                const int slice = remaining < slice_ms ? remaining : slice_ms;
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                remaining -= slice;
            }
            return !st.stop_requested() && !shutdown_requested().load(std::memory_order_relaxed);
        }

        void deferred_scan_body(std::stop_token st) noexcept
        {
            auto &logger = DMK::log();
            const auto map_lookup_addr = resolved_addrs().map_lookup;
            if (!map_lookup_addr)
                return;

            // The initial grace period gives the game a chance to seed the table before the first poll.
            if (!sleep_responsive_ms(st, SCAN_INITIAL_DELAY_MS))
                return;

            std::size_t prev_count = 0;
            int stable_streak = 0;
            int attempt = 0;

            for (;;)
            {
                if (!sleep_responsive_ms(st, SCAN_RETRY_MS))
                    return;

                ++attempt;

                // World-ready gate. user+0xD8 is the controlled-actor pointer. It goes non-null only once the engine
                // finishes the initial world wiring. In main-menu or loading-screen state it stays zero while the
                // IndexedStringA table carries a stable seed of a few engine-internal entries, which otherwise
                // satisfies the stability check below and commits an almost-empty hash map.
                if (read_controlled_actor_ptr() == 0)
                {
                    if (attempt % SCAN_HEARTBEAT_EVERY == 0)
                        logger.debug(
                            "IndexedStringA deferred scan: still waiting "
                            "after {} attempts (controlled actor not yet live)",
                            attempt
                        );
                    stable_streak = 0;
                    prev_count = 0;
                    continue;
                }

                auto runtime_hashes = scan_indexed_string_table(map_lookup_addr);
                const auto cur_count = runtime_hashes.size();

                if (cur_count == 0)
                {
                    // The table is still entirely empty. Reset the streak so stability never commits the empty
                    // state.
                    if (attempt % SCAN_HEARTBEAT_EVERY == 0)
                        logger.debug(
                            "IndexedStringA deferred scan: still waiting after {} attempts (table empty)",
                            attempt
                        );
                    stable_streak = 0;
                    prev_count = 0;
                    continue;
                }

                if (cur_count < MIN_STABLE_COUNT)
                {
                    // Below the minimum-count gate. The world reports ready but the registry published only a
                    // partial table so far. Treat it as still settling.
                    if (cur_count != prev_count)
                    {
                        prev_count = cur_count;
                        logger.trace(
                            "IndexedStringA deferred scan: attempt {}, {} entries (below min {}, awaiting growth)",
                            attempt,
                            cur_count,
                            MIN_STABLE_COUNT
                        );
                    }
                    if (attempt % SCAN_HEARTBEAT_EVERY == 0)
                        logger.debug(
                            "IndexedStringA deferred scan: {} entries "
                            "after {} attempts (below min commit threshold {}, registry still loading)",
                            cur_count,
                            attempt,
                            MIN_STABLE_COUNT
                        );
                    stable_streak = 0;
                    continue;
                }

                if (cur_count != prev_count)
                {
                    // Growing or shrinking: not stable yet, reset streak and update the baseline.
                    stable_streak = 0;
                    prev_count = cur_count;
                    logger.trace(
                        "IndexedStringA deferred scan: attempt {}, {} entries (changed, stability streak reset)",
                        attempt,
                        cur_count
                    );

                    if (attempt % SCAN_HEARTBEAT_EVERY == 0)
                        logger.debug(
                            "IndexedStringA deferred scan: still waiting "
                            "after {} attempts ({} entries currently, table still settling)",
                            attempt,
                            cur_count
                        );
                    continue;
                }

                ++stable_streak;
                if (stable_streak < STABILITY_REQUIRED)
                {
                    logger.trace(
                        "IndexedStringA deferred scan: attempt {}, {} entries (stable {}/{}, awaiting commit)",
                        attempt,
                        cur_count,
                        stable_streak,
                        STABILITY_REQUIRED
                    );

                    if (attempt % SCAN_HEARTBEAT_EVERY == 0)
                        logger.debug(
                            "IndexedStringA deferred scan: {} entries "
                            "after {} attempts (stable streak {}/{}, need {} consecutive identical scans)",
                            cur_count,
                            attempt,
                            stable_streak,
                            STABILITY_REQUIRED,
                            STABILITY_REQUIRED
                        );
                    continue;
                }

                // Re-check stop right before the blocking commit sequence. cleanup_vis_bytes() takes
                // vis_write_mutex with a blocking lock. When shutdown raced past the join, a skipped commit lets the
                // worker exit cleanly.
                if (st.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                    return;

                const auto total_parts = total_part_count();
                // Capture the unresolved parts BEFORE the move of runtime_hashes into the published map, because
                // get_unresolved_parts borrows it.
                auto unresolved = get_unresolved_parts(runtime_hashes);
                const auto resolved_count = total_parts - unresolved.size();

                logger.info(
                    "IndexedStringA deferred scan: stable at {} entries "
                    "across {} consecutive scans, committing ({}/{} resolved, {} attempts)",
                    cur_count,
                    STABILITY_REQUIRED,
                    resolved_count,
                    total_parts,
                    attempt
                );

                set_runtime_hashes(std::move(runtime_hashes));
                rebuild_part_lookup();
                deferred_scan_pending().store(false, std::memory_order_relaxed);

                // Restore any vis bytes set during a prior scan phase so the rebuilt part map can re-apply with correct
                // hashes.
                cleanup_vis_bytes();

                auto &ps = player_state();
                for (int j = 0; j < MAX_PROTAGONISTS; ++j)
                    ps.armor_injected[j].store(false, std::memory_order_relaxed);
                needs_direct_write().store(true, std::memory_order_release);

                if (!unresolved.empty())
                    lazy_probe_pending().store(true, std::memory_order_relaxed);
                return;
            }
        }

        void lazy_probe_body(std::stop_token st) noexcept
        {
            auto &logger = DMK::log();
            const auto map_lookup_addr = resolved_addrs().map_lookup;
            int probe_count = 0;

            // Track the lowest unresolved count committed so far. A scan that drops the unresolved count below this
            // baseline means new INI parts turned resolvable since the last commit, so the worker publishes even
            // while some still miss. One typo in the INI therefore no longer blocks the rest of the part map.
            // Seeded with size_t-max so the first non-empty scan always wins.
            std::size_t best_unresolved = std::numeric_limits<std::size_t>::max();

            // Signal value consumed by the previous tick.
            //
            // The signal doubles as the interval throttle: it carries the timestamp it was armed at, and the mid-hook
            // re-arms only when it reads 0 or a value older than LAZY_PROBE_INTERVAL_MS. Consuming it by comparison
            // keeps that throttle intact. Clearing it instead would present the producer with the "never armed"
            // sentinel, which it re-arms on immediately, collapsing the real interval to this loop's sleep period.
            int64_t last_signal = 0;

            logger.info("Lazy probe started for demand-loaded parts (interval: {}s)", LAZY_PROBE_INTERVAL_MS / 1000);

            while (lazy_probe_pending().load(std::memory_order_relaxed))
            {
                if (!sleep_responsive_ms(st, static_cast<int>(LAZY_PROBE_TICK_MS)))
                    return;

                const auto signal = lazy_probe_signal().load(std::memory_order_relaxed);
                if (signal == 0 || signal == last_signal)
                    continue;
                last_signal = signal;

                ++probe_count;
                logger.trace("Lazy probe #{}: scanning IndexedStringA table", probe_count);

                auto runtime_hashes = scan_indexed_string_table(map_lookup_addr);
                if (runtime_hashes.empty())
                    continue;

                auto unresolved = get_unresolved_parts(runtime_hashes);
                const auto unresolved_count = unresolved.size();
                const bool fully_resolved = unresolved.empty();
                const bool made_progress = unresolved_count < best_unresolved;

                if (!made_progress)
                {
                    // No new INI parts resolved since the last commit. Common stable state on saves whose INI carries
                    // unresolvable entries (typos or parts the game never registers). Keep the probe alive in case the
                    // registry grows later but skip the rebuild work for this tick.
                    logger.trace(
                        "Lazy probe #{}: {} parts unresolved (no progress since last commit)",
                        probe_count,
                        unresolved_count
                    );
                    continue;
                }

                // Same stop-check rationale as deferred_scan_body. cleanup_vis_bytes() takes vis_write_mutex with a
                // blocking lock, which stalls behind the resolve-poll worker's try-locked critical section during
                // shutdown.
                if (st.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                    return;

                const auto new_hash_count = runtime_hashes.size();
                set_runtime_hashes(std::move(runtime_hashes));
                rebuild_part_lookup();
                cleanup_vis_bytes();
                auto &ps = player_state();
                for (int j = 0; j < MAX_PROTAGONISTS; ++j)
                    ps.armor_injected[j].store(false, std::memory_order_relaxed);
                needs_direct_write().store(true, std::memory_order_release);

                best_unresolved = unresolved_count;

                if (fully_resolved)
                {
                    lazy_probe_pending().store(false, std::memory_order_relaxed);
                    logger.info("Lazy probe resolved all remaining parts ({} probes)", probe_count);
                    return;
                }

                logger.info(
                    "Lazy probe #{}: committed {} runtime hashes; "
                    "{} INI parts still unresolved (will keep polling for late registrations)",
                    probe_count,
                    new_hash_count,
                    unresolved_count
                );
            }
        }

        // Walks WorldSystem -> ActorManager -> UserActor -> +0xD8 and returns the currently-controlled
        // ClientChildOnlyInGameActor pointer, or 0 on any fault or unresolved intermediate. The pointer rotates on
        // three game events the poll thread observes.
        //
        // - World or save load: the UserActor reallocates, so user+0xD8 resolves through a different chain entirely.
        // - In-session character swap: the UserActor stays and the +0xD8 slot rotates to the new party member's
        //   actor.
        // - Controlled-actor teardown: the read returns 0 while the engine wires the new actor up.
        //
        // All three settle on a stable non-zero pointer on success, so a compare of the current read against the
        // previous tick catches every case and needs no separate UserActor and actor watchers. The walk is fault
        // guarded, because every intermediate deref can land on a half-torn state during a rotation.
        std::uintptr_t read_controlled_actor_ptr() noexcept
        {
            auto &addrs = resolved_addrs();
            if (!addrs.world_system)
                return 0;
            // The walk steps WorldSystem -> ActorManager -> UserActor -> controlled-actor SLOT under one fault
            // guard, and the trailing read performs the terminal deref, so a half-torn rotation state faults safely
            // instead of a crash. The final floor rejects the not-yet-wired-up sentinel range. Layout offsets come
            // from CDCore::actor_chain_offsets (controlled_char.hpp), the single authority shared with LiveTransmog and
            // with CDCore's own resolver.
            namespace ac = CDCore::actor_chain_offsets;
            const auto r = DMK::memory::walk(
                               DMK::Address{addrs.world_system},
                               std::array<std::ptrdiff_t, 4>{
                                   0x00,
                                   ac::WORLD_SYSTEM_TO_ACTOR_MANAGER,
                                   ac::ACTOR_MANAGER_TO_USER_ACTOR,
                                   ac::USER_ACTOR_TO_CONTROLLED,
                               }
            )
                               .and_then([](DMK::Address leaf) { return DMK::memory::read<std::uintptr_t>(leaf); });
            return (r && *r >= DMK::memory::USERSPACE_PTR_MIN) ? *r : 0;
        }

        // Fires resolve_player_vis_ctrls on either trigger: the controlled-actor pointer rotates (a radial swap or a
        // save-load), or the player snapshot count changes (Damiane or Oongka summoned mid-session while the user
        // stays on Kliff).
        //
        // Without the snapshot trigger a mid-session summon never re-resolves. The controlled actor stays put and
        // ps.count sticks at its pre-summon value, so DirectWrite hides gear for the original character only. The
        // snapshot-count poll uses a (size_t)-1 uninit sentinel so the first observation does not double-fire.
        void resolve_poll_body(std::stop_token st) noexcept
        {
            auto &logger = DMK::log();
            logger.info("Resolve poll thread started (interval: {}ms)", RESOLVE_POLL_INTERVAL_MS);

            std::uintptr_t prev_actor = 0;
            constexpr std::size_t snapshot_count_uninit = static_cast<std::size_t>(-1);
            std::size_t prev_snapshot_count = snapshot_count_uninit;

            while (!st.stop_requested() && !shutdown_requested().load(std::memory_order_relaxed))
            {
                if (!sleep_responsive_ms(st, static_cast<int>(RESOLVE_POLL_INTERVAL_MS)))
                    return;

                // Defer until the AOB-resolved singletons are populated. read_controlled_actor_ptr carries an
                // equivalent guard, and this check avoids the guarded-walk cost on every early tick before init
                // completes.
                auto &addrs = resolved_addrs();
                if (!addrs.world_system || !addrs.child_actor_vtbl)
                    continue;

                const auto cur_actor = read_controlled_actor_ptr();
                const bool actor_rotated = (cur_actor != prev_actor);

                // Any change in roster size re-resolves, in both directions. A shrink matters as much as a growth. A
                // despawned companion leaves its vis ctrl published and the direct-write path then keeps writing
                // through that stale pointer. Nothing else observes a despawn, because the controlled actor does not
                // rotate for it, so this is the only trigger that catches one.
                std::array<CDCore::BodyCacheEntry, MAX_PROTAGONISTS> snap{};
                const auto cur_snapshot_count = CDCore::snapshot_body_cache(snap.data(), snap.size());
                const bool roster_changed =
                    (prev_snapshot_count != snapshot_count_uninit && cur_snapshot_count != prev_snapshot_count);

                const auto prev_snapshot_count_for_log = prev_snapshot_count;
                if (actor_rotated)
                    prev_actor = cur_actor;
                prev_snapshot_count = cur_snapshot_count;

                if (!actor_rotated && !roster_changed)
                    continue;

                if (roster_changed)
                    logger.info(
                        "Player roster changed {} -> {}; re-resolving vis-ctrls",
                        prev_snapshot_count_for_log,
                        cur_snapshot_count
                    );

                resolve_player_vis_ctrls();
            }
        }
    } // namespace

    void launch_deferred_scan() noexcept
    {
        if (!deferred_scan_pending().load(std::memory_order_relaxed))
            return;
        if (!resolved_addrs().map_lookup)
            return;

        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lk(s_workers_mtx);
        s_deferred_scan_worker =
            std::make_unique<DetourModKit::StoppableWorker>("EH_DeferredScan", &deferred_scan_body);
    }

    void launch_lazy_probe() noexcept
    {
        if (!lazy_probe_pending().load(std::memory_order_relaxed))
            return;

        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lk(s_workers_mtx);
        s_lazy_probe_worker = std::make_unique<DetourModKit::StoppableWorker>("EH_LazyProbe", &lazy_probe_body);
    }

    void launch_resolve_poll() noexcept
    {
        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lk(s_workers_mtx);
        s_resolve_poll_worker = std::make_unique<DetourModKit::StoppableWorker>("EH_ResolvePoll", &resolve_poll_body);
    }

    void join_background_threads()
    {
        // shutdown() flips the process-wide shutdown_requested() flag before this call. The explicit shutdown() on
        // each worker also calls request_stop() on its stop_token, so the bodies exit promptly.
        //
        // Lock ordering: extract each unique_ptr into a local under s_workers_mtx, then RELEASE the mutex before the
        // join. A join under s_workers_mtx deadlocks against the mid-hook path on the game thread. An EquipVisCheck
        // tick calls launch_lazy_probe(), which blocks on s_workers_mtx.lock(). Meanwhile the lazy-probe worker body
        // calls cleanup_vis_bytes(), whose blocking lock on vis_write_mutex stalls behind the resolve-poll worker's
        // try_lock-held critical section in resolve_player_vis_ctrls. With the lock held, shutdown waits on the
        // lazy-probe join while the game thread waits on s_workers_mtx, forever.
        //
        // Request stop on all three first so the bodies unwind concurrently across the sequential joins below. That
        // shaves wall-clock shutdown latency and leaves the join order unchanged.
        auto &logger = DMK::log();

        std::unique_ptr<DetourModKit::StoppableWorker> deferred_local;
        std::unique_ptr<DetourModKit::StoppableWorker> lazy_local;
        std::unique_ptr<DetourModKit::StoppableWorker> resolve_local;
        {
            std::lock_guard<std::mutex> lk(s_workers_mtx);
            deferred_local = std::move(s_deferred_scan_worker);
            lazy_local = std::move(s_lazy_probe_worker);
            resolve_local = std::move(s_resolve_poll_worker);
        }

        if (deferred_local)
            deferred_local->request_stop();
        if (lazy_local)
            lazy_local->request_stop();
        if (resolve_local)
            resolve_local->request_stop();

        if (resolve_local)
        {
            logger.info("{} shutdown: joining resolve-poll worker", MOD_NAME);
            resolve_local->shutdown();
        }
        if (lazy_local)
        {
            logger.info("{} shutdown: joining lazy-probe worker", MOD_NAME);
            lazy_local->shutdown();
        }
        if (deferred_local)
        {
            logger.info("{} shutdown: joining deferred-scan worker", MOD_NAME);
            deferred_local->shutdown();
        }
    }

} // namespace EquipHide
