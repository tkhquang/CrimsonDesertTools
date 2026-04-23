#include "background_threads.hpp"
#include "indexed_string_table.hpp"
#include "player_detection.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <chrono>
#include <mutex>
#include <thread>

namespace EquipHide
{
    static std::mutex s_bgThreadMtx;
    static std::thread s_deferredScanThread;
    static std::thread s_lazyProbeThread;
    static std::thread s_resolvePollThread;

    // --- Deferred IndexedStringA scan ---
    static constexpr int k_maxScanAttempts = 10;
    static constexpr int k_scanRetryMs = 2000;
    static constexpr int k_scanIdleRetryMs = 10000;
    static constexpr int k_scanInitialDelayMs = 8000;

    static void deferred_scan_thread_fn() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        auto mapLookupAddr = resolved_addrs().mapLookup;

        int realAttempts = 0;
        bool tableReady = false;
        for (;;)
        {
            if (shutdown_requested().load(std::memory_order_relaxed))
                return;

            int sleepMs;
            if (realAttempts == 0 && !tableReady)
                sleepMs = k_scanInitialDelayMs;
            else if (!tableReady)
                sleepMs = k_scanIdleRetryMs;
            else
                sleepMs = k_scanRetryMs;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

            auto runtimeHashes = scan_indexed_string_table(mapLookupAddr);
            if (runtimeHashes.empty())
                continue;

            const auto totalParts = total_part_count();
            auto unresolved = get_unresolved_parts(runtimeHashes);
            const auto resolvedCount = totalParts - unresolved.size();

            // Table not meaningfully populated yet -- keep waiting
            // without burning attempt budget.
            if (resolvedCount < 10)
                continue;

            tableReady = true;
            ++realAttempts;

            // Accept results when >=90% resolved or attempt budget spent.
            constexpr auto k_minResolvePct = 90;
            const bool enough = (resolvedCount * 100 / totalParts) >= k_minResolvePct;

            if (enough || realAttempts >= k_maxScanAttempts)
            {
                set_runtime_hashes(std::move(runtimeHashes));
                rebuild_part_lookup();
                deferred_scan_pending().store(false, std::memory_order_relaxed);

                // Restore any vis bytes set during a prior scan phase so the
                // rebuilt part map can re-apply with correct hashes.
                cleanup_vis_bytes();

                auto &ps = player_state();
                for (int j = 0; j < k_maxProtagonists; ++j)
                    ps.armorInjected[j].store(false, std::memory_order_relaxed);
                needs_direct_write().store(true, std::memory_order_relaxed);

                logger.info("Deferred scan complete: {}/{} resolved ({} attempts)",
                            resolvedCount, totalParts, realAttempts);

                if (!unresolved.empty())
                    lazy_probe_pending().store(true, std::memory_order_relaxed);
                return;
            }

            logger.trace("Deferred scan attempt {}: {}/{} resolved, retrying",
                         realAttempts, resolvedCount, totalParts);
        }
    }

    void launch_deferred_scan() noexcept
    {
        if (!deferred_scan_pending().load(std::memory_order_relaxed))
            return;
        if (!resolved_addrs().mapLookup)
            return;

        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        {
            std::lock_guard<std::mutex> lk(s_bgThreadMtx);
            s_deferredScanThread = std::thread(deferred_scan_thread_fn);
        }
    }

    // --- Lazy re-probe for demand-loaded IndexedStringA entries ---
    static void lazy_probe_thread_fn() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        auto mapLookupAddr = resolved_addrs().mapLookup;
        int probeCount = 0;

        logger.info("Lazy probe started for demand-loaded parts "
                    "(interval: {}s)",
                    k_lazyProbeIntervalMs / 1000);

        while (lazy_probe_pending().load(std::memory_order_relaxed))
        {
            if (shutdown_requested().load(std::memory_order_relaxed))
                return;
            std::this_thread::sleep_for(std::chrono::seconds(5));

            auto signal = lazy_probe_signal().load(std::memory_order_relaxed);
            if (signal == 0)
                continue;

            ++probeCount;
            logger.trace("Lazy probe #{}: scanning IndexedStringA table", probeCount);

            auto runtimeHashes = scan_indexed_string_table(mapLookupAddr);
            if (runtimeHashes.empty())
                continue;

            auto unresolved = get_unresolved_parts(runtimeHashes);
            if (unresolved.empty())
            {
                set_runtime_hashes(std::move(runtimeHashes));
                rebuild_part_lookup();
                cleanup_vis_bytes();
                lazy_probe_pending().store(false, std::memory_order_relaxed);
                auto &ps = player_state();
                for (int j = 0; j < k_maxProtagonists; ++j)
                    ps.armorInjected[j].store(false, std::memory_order_relaxed);
                needs_direct_write().store(true, std::memory_order_relaxed);
                logger.info("Lazy probe resolved all remaining parts "
                            "({} probes)",
                            probeCount);
                return;
            }

            logger.trace("Lazy probe #{}: {} parts still unresolved",
                         probeCount, unresolved.size());

            lazy_probe_signal().store(0, std::memory_order_relaxed);
        }
    }

    void launch_lazy_probe() noexcept
    {
        if (!lazy_probe_pending().load(std::memory_order_relaxed))
            return;

        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        {
            std::lock_guard<std::mutex> lk(s_bgThreadMtx);
            s_lazyProbeThread = std::thread(lazy_probe_thread_fn);
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
    static std::uintptr_t read_controlled_actor_ptr_seh() noexcept
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

    // Fires resolve_player_vis_ctrls only when the controlled-actor
    // pointer has rotated since the previous tick. Decoupled from the
    // EquipVisCheck hook's event stream so swap detection lands within
    // one poll interval regardless of what the hook is doing:
    // check_player_filter's in-hook poll (every 512 EquipVisCheck
    // ticks on v1.03.01) is still in place but relies on classified
    // CD_* part hashes reaching the hook, which is not guaranteed on
    // v1.04.00 during cold load or between character swaps.
    //
    // Change-gated rather than unconditional: the full chain walk +
    // body enumeration + part-map rebuild is skipped on quiescent
    // ticks. The two-pointer compare here is the same signal
    // resolve_player_vis_ctrls uses internally
    // (s_prevControlledActor); lifting it into the thread lets the
    // heavier work run only when needed. The initial cold-load
    // iteration fires once when prevActor is still 0 and the chain
    // first resolves to a valid actor, populating ps.count before
    // the EquipVisCheck hook's count==0 inline fallback can run.
    //
    // resolve_player_vis_ctrls is SEH-guarded and idempotent; the
    // thread exits on shutdown_requested().
    static void resolve_poll_thread_fn() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        logger.info("Resolve poll thread started (interval: {}ms)",
                    k_resolvePollIntervalMs);

        std::uintptr_t prevActor = 0;

        while (!shutdown_requested().load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(k_resolvePollIntervalMs));
            if (shutdown_requested().load(std::memory_order_relaxed))
                return;

            // Defer until the AOB-resolved singletons are populated.
            // read_controlled_actor_ptr_seh has an equivalent guard
            // but checking here avoids the SEH frame cost on every
            // early tick before init completes.
            auto &addrs = resolved_addrs();
            if (!addrs.worldSystem || !addrs.childActorVtbl)
                continue;

            const auto curActor = read_controlled_actor_ptr_seh();
            if (curActor == prevActor)
                continue;
            prevActor = curActor;

            resolve_player_vis_ctrls();
        }
    }

    void launch_resolve_poll() noexcept
    {
        static std::atomic<bool> s_launched{false};
        if (s_launched.exchange(true, std::memory_order_relaxed))
            return;

        {
            std::lock_guard<std::mutex> lk(s_bgThreadMtx);
            s_resolvePollThread = std::thread(resolve_poll_thread_fn);
        }
    }

    void join_background_threads()
    {
        std::lock_guard<std::mutex> lk(s_bgThreadMtx);
        if (s_deferredScanThread.joinable())
            s_deferredScanThread.join();
        if (s_lazyProbeThread.joinable())
            s_lazyProbeThread.join();
        if (s_resolvePollThread.joinable())
            s_resolvePollThread.join();
    }

} // namespace EquipHide
