#include "background_threads.hpp"
#include "constants.hpp"
#include "indexed_string_table.hpp"
#include "player_detection.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>
#include <DetourModKit/worker.hpp>

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
        // s_workersMtx serialises the lazy-launch arms; each worker
        // pointer is set under the lock in launch_*(), then read
        // unlocked in join_background_threads(). The atomic launch
        // guards prevent a second launch_*() call from racing the
        // first, so the unique_ptr writes themselves see no contention
        // beyond the launch site.
        std::mutex s_workersMtx;
        std::unique_ptr<DetourModKit::StoppableWorker> s_deferredScanWorker;
        std::unique_ptr<DetourModKit::StoppableWorker> s_lazyProbeWorker;
        std::unique_ptr<DetourModKit::StoppableWorker> s_resolvePollWorker;

        // --- Deferred IndexedStringA scan tuning ---
        // Convergence rests on two gates plus a stability window:
        //   (1) World-ready: user+0xD8 must hold a non-null controlled
        //       actor. In main-menu / loading-screen state the
        //       IndexedStringA table carries a tiny engine-internal
        //       seed (~3 entries) that is naturally "stable" for
        //       many seconds, which would false-trigger a pure
        //       stability check. Walking the same chain EquipHide
        //       already polls in resolve_poll_body lets us refuse to
        //       commit until the world is actually live.
        //   (2) Minimum count: even after the chain reports a
        //       controlled actor we refuse to commit below
        //       k_minStableCount so a torn / partially-loaded
        //       registry cannot snapshot a sub-50 entry table that
        //       happens to repeat itself across consecutive polls.
        //   (3) Stability window: with both gates satisfied, k_-
        //       stabilityRequired identical scans confirm the table
        //       has finished growing for this load.
        constexpr int         k_scanRetryMs       = 2000;
        constexpr int         k_scanInitialDelayMs = 8000;
        constexpr int         k_scanWarnEvery     = 30;
        constexpr int         k_stabilityRequired = 5;
        constexpr std::size_t k_minStableCount    = 32;

        // Forward declaration: world-ready probe lives below for
        // proximity to the resolve-poll thread that consumes it, but
        // the deferred scan body needs to call it as a commit gate.
        std::uintptr_t read_controlled_actor_ptr_seh() noexcept;

        // Sleeps in short slices and observes both the StoppableWorker
        // stop_token and the legacy shutdown_requested() flag so the
        // worker exits within ~50ms of either signal regardless of how
        // long the per-iteration retry delay is.
        bool sleep_responsive_ms(std::stop_token st, int totalMs) noexcept
        {
            constexpr int sliceMs = 50;
            int remaining = totalMs;
            while (remaining > 0)
            {
                if (st.stop_requested() ||
                    shutdown_requested().load(std::memory_order_relaxed))
                    return false;
                const int slice = remaining < sliceMs ? remaining : sliceMs;
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                remaining -= slice;
            }
            return !st.stop_requested() &&
                   !shutdown_requested().load(std::memory_order_relaxed);
        }

        void deferred_scan_body(std::stop_token st) noexcept
        {
            auto &logger = DMK::Logger::get_instance();
            const auto mapLookupAddr = resolved_addrs().mapLookup;
            if (!mapLookupAddr)
                return;

            // Initial grace mirrors LT's k_nametableInitialDelayMs so the
            // game has a chance to seed the table before we start polling.
            if (!sleep_responsive_ms(st, k_scanInitialDelayMs))
                return;

            std::size_t prevCount = 0;
            int stableStreak = 0;
            int attempt = 0;

            for (;;)
            {
                if (!sleep_responsive_ms(st, k_scanRetryMs))
                    return;

                ++attempt;

                // World-ready gate. user+0xD8 is the controlled-actor
                // pointer; it goes non-null only once the engine has
                // finished initial world wiring. In main-menu /
                // loading-screen state it stays zero while the
                // IndexedStringA table carries a stable seed of a
                // few engine-internal entries that would otherwise
                // satisfy the stability check below and commit an
                // almost-empty hash map.
                if (read_controlled_actor_ptr_seh() == 0)
                {
                    if (attempt % k_scanWarnEvery == 0)
                        logger.warning(
                            "IndexedStringA deferred scan: still waiting "
                            "after {} attempts (controlled actor not yet "
                            "live)",
                            attempt);
                    stableStreak = 0;
                    prevCount = 0;
                    continue;
                }

                auto runtimeHashes = scan_indexed_string_table(mapLookupAddr);
                const auto curCount = runtimeHashes.size();

                if (curCount == 0)
                {
                    // Table still entirely empty: reset streak so we never
                    // commit the empty state on stability.
                    if (attempt % k_scanWarnEvery == 0)
                        logger.warning(
                            "IndexedStringA deferred scan: still waiting "
                            "after {} attempts (table empty)",
                            attempt);
                    stableStreak = 0;
                    prevCount = 0;
                    continue;
                }

                if (curCount < k_minStableCount)
                {
                    // Below the minimum-count gate. The world reports
                    // ready but the registry has only published a
                    // partial table so far; treat as still settling.
                    if (curCount != prevCount)
                    {
                        prevCount = curCount;
                        logger.trace(
                            "IndexedStringA deferred scan: attempt {}, "
                            "{} entries (below min {}, awaiting growth)",
                            attempt, curCount, k_minStableCount);
                    }
                    if (attempt % k_scanWarnEvery == 0)
                        logger.warning(
                            "IndexedStringA deferred scan: {} entries "
                            "after {} attempts (below min commit "
                            "threshold {}, registry still loading)",
                            curCount, attempt, k_minStableCount);
                    stableStreak = 0;
                    continue;
                }

                if (curCount != prevCount)
                {
                    // Growing or shrinking: not stable yet, reset streak
                    // and update the baseline.
                    stableStreak = 0;
                    prevCount = curCount;
                    logger.trace(
                        "IndexedStringA deferred scan: attempt {}, {} "
                        "entries (changed, stability streak reset)",
                        attempt, curCount);

                    if (attempt % k_scanWarnEvery == 0)
                        logger.warning(
                            "IndexedStringA deferred scan: still waiting "
                            "after {} attempts ({} entries currently, "
                            "table still settling)",
                            attempt, curCount);
                    continue;
                }

                ++stableStreak;
                if (stableStreak < k_stabilityRequired)
                {
                    logger.trace(
                        "IndexedStringA deferred scan: attempt {}, {} "
                        "entries (stable {}/{}, awaiting commit)",
                        attempt, curCount, stableStreak,
                        k_stabilityRequired);

                    if (attempt % k_scanWarnEvery == 0)
                        logger.warning(
                            "IndexedStringA deferred scan: {} entries "
                            "after {} attempts (stable streak {}/{}, "
                            "need {} consecutive identical scans)",
                            curCount, attempt, stableStreak,
                            k_stabilityRequired, k_stabilityRequired);
                    continue;
                }

                // Re-check stop right before the blocking commit
                // sequence. cleanup_vis_bytes() blocking-locks
                // vis_write_mutex; if shutdown raced past join here,
                // skipping the commit lets the worker exit cleanly.
                if (st.stop_requested() ||
                    shutdown_requested().load(std::memory_order_relaxed))
                    return;

                const auto totalParts = total_part_count();
                // Capture unresolved parts BEFORE moving runtimeHashes into
                // the published map, since get_unresolved_parts borrows it.
                auto unresolved = get_unresolved_parts(runtimeHashes);
                const auto resolvedCount = totalParts - unresolved.size();

                logger.info(
                    "IndexedStringA deferred scan: stable at {} entries "
                    "across {} consecutive scans, committing "
                    "({}/{} resolved, {} attempts)",
                    curCount, k_stabilityRequired,
                    resolvedCount, totalParts, attempt);

                set_runtime_hashes(std::move(runtimeHashes));
                rebuild_part_lookup();
                deferred_scan_pending().store(
                    false, std::memory_order_relaxed);

                // Restore any vis bytes set during a prior scan phase
                // so the rebuilt part map can re-apply with correct
                // hashes.
                cleanup_vis_bytes();

                auto &ps = player_state();
                for (int j = 0; j < k_maxProtagonists; ++j)
                    ps.armorInjected[j].store(
                        false, std::memory_order_relaxed);
                needs_direct_write().store(
                    true, std::memory_order_relaxed);

                if (!unresolved.empty())
                    lazy_probe_pending().store(
                        true, std::memory_order_relaxed);
                return;
            }
        }

        void lazy_probe_body(std::stop_token st) noexcept
        {
            auto &logger = DMK::Logger::get_instance();
            const auto mapLookupAddr = resolved_addrs().mapLookup;
            int probeCount = 0;

            // Track the lowest unresolved count we have committed so
            // far. A scan that drops the unresolved count below this
            // baseline means new INI parts became resolvable since
            // the last commit, so we publish even if some still miss
            // -- a single typo in the INI no longer blocks the rest
            // of the part map from being applied. Seeded with
            // size_t-max so the first non-empty scan always wins.
            std::size_t bestUnresolved =
                std::numeric_limits<std::size_t>::max();

            logger.info("Lazy probe started for demand-loaded parts "
                        "(interval: {}s)",
                        k_lazyProbeIntervalMs / 1000);

            while (lazy_probe_pending().load(std::memory_order_relaxed))
            {
                if (!sleep_responsive_ms(st, 5000))
                    return;

                const auto signal =
                    lazy_probe_signal().load(std::memory_order_relaxed);
                if (signal == 0)
                    continue;

                ++probeCount;
                logger.trace(
                    "Lazy probe #{}: scanning IndexedStringA table",
                    probeCount);

                auto runtimeHashes = scan_indexed_string_table(mapLookupAddr);
                if (runtimeHashes.empty())
                    continue;

                auto unresolved = get_unresolved_parts(runtimeHashes);
                const auto unresolvedCount = unresolved.size();
                const bool fullyResolved = unresolved.empty();
                const bool madeProgress = unresolvedCount < bestUnresolved;

                if (!madeProgress)
                {
                    // No new INI parts resolved since the last
                    // commit. Common stable state on saves whose
                    // INI carries unresolvable entries (typos or
                    // parts the game never registers). Keep the
                    // probe alive in case the registry grows later
                    // but skip the rebuild work for this tick.
                    logger.trace(
                        "Lazy probe #{}: {} parts unresolved "
                        "(no progress since last commit)",
                        probeCount, unresolvedCount);
                    lazy_probe_signal().store(
                        0, std::memory_order_relaxed);
                    continue;
                }

                // Same stop-check rationale as deferred_scan_body:
                // cleanup_vis_bytes() takes vis_write_mutex blocking,
                // which can stall behind the resolve-poll worker's
                // try-locked critical section during shutdown.
                if (st.stop_requested() ||
                    shutdown_requested().load(std::memory_order_relaxed))
                    return;

                const auto newHashCount = runtimeHashes.size();
                set_runtime_hashes(std::move(runtimeHashes));
                rebuild_part_lookup();
                cleanup_vis_bytes();
                auto &ps = player_state();
                for (int j = 0; j < k_maxProtagonists; ++j)
                    ps.armorInjected[j].store(
                        false, std::memory_order_relaxed);
                needs_direct_write().store(
                    true, std::memory_order_relaxed);

                bestUnresolved = unresolvedCount;

                if (fullyResolved)
                {
                    lazy_probe_pending().store(
                        false, std::memory_order_relaxed);
                    logger.info(
                        "Lazy probe resolved all remaining parts "
                        "({} probes)",
                        probeCount);
                    return;
                }

                logger.info(
                    "Lazy probe #{}: committed {} runtime hashes; "
                    "{} INI parts still unresolved (will keep "
                    "polling for late registrations)",
                    probeCount, newHashCount, unresolvedCount);

                lazy_probe_signal().store(0, std::memory_order_relaxed);
            }
        }

        // Walks WorldSystem -> ActorManager -> UserActor -> +0xD8 and
        // returns the currently-controlled ClientChildOnlyInGameActor
        // pointer, or 0 on any fault or unresolved intermediate. The
        // pointer rotates on three game events the poll thread observes:
        //
        //   1. World / save load          -- UserActor reallocates, so
        //                                    user+0xD8 resolves through a
        //                                    different chain entirely.
        //   2. In-session character swap  -- UserActor stays; the +0xD8
        //                                    slot rotates to the new
        //                                    party member's actor.
        //   3. Controlled-actor teardown  -- returns 0 while the new
        //                                    actor is being wired up.
        //
        // All three settle on a stable non-zero pointer on success, so
        // comparing the current read against the previous tick catches
        // every case without needing separate UA and actor watchers.
        // SEH-guarded because every intermediate deref may land on a
        // half-torn state during a rotation.
        std::uintptr_t read_controlled_actor_ptr_seh() noexcept
        {
            auto &addrs = resolved_addrs();
            if (!addrs.worldSystem)
                return 0;
            __try
            {
                auto ws = *reinterpret_cast<uintptr_t *>(addrs.worldSystem);
                if (ws < 0x10000)
                    return 0;
                auto am = *reinterpret_cast<uintptr_t *>(ws + 0x30);
                if (am < 0x10000)
                    return 0;
                auto user = *reinterpret_cast<uintptr_t *>(am + 0x28);
                if (user < 0x10000)
                    return 0;
                auto actor = *reinterpret_cast<uintptr_t *>(user + 0xD8);
                if (actor < 0x10000)
                    return 0;
                return actor;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // Fires resolve_player_vis_ctrls when either:
        //   (a) the controlled-actor pointer rotates (radial swap or
        //       save-load), or
        //   (b) the player snapshot count grows (Damiane / Oongka
        //       summoned mid-session while the user stays on Kliff).
        //
        // Without (b) a mid-session summon never triggers a re-resolve:
        // the controlled actor stays put and ps.count remains stuck at
        // its pre-summon value, so DirectWrite keeps hiding gear for
        // only the original character. The snapshot-count poll matches
        // LT's roster-grew trigger and uses the (size_t)-1 uninit
        // sentinel so the first observation does not double-fire.
        void resolve_poll_body(std::stop_token st) noexcept
        {
            auto &logger = DMK::Logger::get_instance();
            logger.info("Resolve poll thread started (interval: {}ms)",
                        k_resolvePollIntervalMs);

            std::uintptr_t prevActor = 0;
            constexpr std::size_t kSnapshotCountUninit =
                static_cast<std::size_t>(-1);
            std::size_t prevSnapshotCount = kSnapshotCountUninit;

            while (!st.stop_requested() &&
                   !shutdown_requested().load(std::memory_order_relaxed))
            {
                if (!sleep_responsive_ms(
                        st, static_cast<int>(k_resolvePollIntervalMs)))
                    return;

                // Defer until the AOB-resolved singletons are populated.
                // read_controlled_actor_ptr_seh has an equivalent guard
                // but checking here avoids the SEH frame cost on every
                // early tick before init completes.
                auto &addrs = resolved_addrs();
                if (!addrs.worldSystem || !addrs.childActorVtbl)
                    continue;

                const auto curActor = read_controlled_actor_ptr_seh();
                const bool actorRotated = (curActor != prevActor);

                std::array<CDCore::BodyCacheEntry, k_maxProtagonists> snap{};
                const auto curSnapshotCount =
                    CDCore::snapshot_body_cache(
                        snap.data(), snap.size());
                const bool rosterGrew =
                    (prevSnapshotCount != kSnapshotCountUninit &&
                     curSnapshotCount > prevSnapshotCount);

                const auto prevSnapshotCountForLog = prevSnapshotCount;
                if (actorRotated)
                    prevActor = curActor;
                prevSnapshotCount = curSnapshotCount;

                if (!actorRotated && !rosterGrew)
                    continue;

                if (rosterGrew)
                    logger.info(
                        "Player roster grew {} -> {}; re-resolving "
                        "vis-ctrls",
                        prevSnapshotCountForLog, curSnapshotCount);

                resolve_player_vis_ctrls();
            }
        }
    } // namespace

    void launch_deferred_scan() noexcept
    {
        if (!deferred_scan_pending().load(std::memory_order_relaxed))
            return;
        if (!resolved_addrs().mapLookup)
            return;

        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lk(s_workersMtx);
        s_deferredScanWorker =
            std::make_unique<DetourModKit::StoppableWorker>(
                "EH_DeferredScan", &deferred_scan_body);
    }

    void launch_lazy_probe() noexcept
    {
        if (!lazy_probe_pending().load(std::memory_order_relaxed))
            return;

        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lk(s_workersMtx);
        s_lazyProbeWorker =
            std::make_unique<DetourModKit::StoppableWorker>(
                "EH_LazyProbe", &lazy_probe_body);
    }

    void launch_resolve_poll() noexcept
    {
        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lk(s_workersMtx);
        s_resolvePollWorker =
            std::make_unique<DetourModKit::StoppableWorker>(
                "EH_ResolvePoll", &resolve_poll_body);
    }

    void join_background_threads()
    {
        // shutdown() flips the legacy shutdown_requested() flag before
        // calling here; explicit shutdown() on each worker also
        // request_stop()s the stop_token so bodies exit promptly.
        //
        // Lock-ordering: extract each unique_ptr into a local under
        // s_workersMtx, then RELEASE the mutex before joining. Joining
        // while holding s_workersMtx deadlocks against the mid-hook
        // path on the game thread: an EquipVisCheck tick calls
        // launch_lazy_probe() which blocks on s_workersMtx.lock();
        // meanwhile the lazy-probe worker body itself calls
        // cleanup_vis_bytes() (blocking lock on vis_write_mutex) which
        // can stall behind the resolve-poll worker's try_lock-held
        // critical section in resolve_player_vis_ctrls. With the lock
        // held, shutdown waits on the lazy-probe join while the game
        // thread waits on s_workersMtx -- forever.
        //
        // Request stop on all three first so the bodies start unwinding
        // concurrently while we sequentially join, shaving wall-clock
        // shutdown latency without changing the join order.
        auto &logger = DMK::Logger::get_instance();

        std::unique_ptr<DetourModKit::StoppableWorker> deferredLocal;
        std::unique_ptr<DetourModKit::StoppableWorker> lazyLocal;
        std::unique_ptr<DetourModKit::StoppableWorker> resolveLocal;
        {
            std::lock_guard<std::mutex> lk(s_workersMtx);
            deferredLocal = std::move(s_deferredScanWorker);
            lazyLocal = std::move(s_lazyProbeWorker);
            resolveLocal = std::move(s_resolvePollWorker);
        }

        if (deferredLocal)
            deferredLocal->request_stop();
        if (lazyLocal)
            lazyLocal->request_stop();
        if (resolveLocal)
            resolveLocal->request_stop();

        if (resolveLocal)
        {
            logger.info("{} shutdown: joining resolve-poll worker", MOD_NAME);
            resolveLocal->shutdown();
        }
        if (lazyLocal)
        {
            logger.info("{} shutdown: joining lazy-probe worker", MOD_NAME);
            lazyLocal->shutdown();
        }
        if (deferredLocal)
        {
            logger.info("{} shutdown: joining deferred-scan worker", MOD_NAME);
            deferredLocal->shutdown();
        }
    }

} // namespace EquipHide
