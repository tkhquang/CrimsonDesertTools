#include "background_threads.hpp"
#include "indexed_string_table.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <DetourModKit.hpp>

#include <chrono>
#include <mutex>
#include <thread>

namespace EquipHide
{
    static std::mutex s_bgThreadMtx;
    static std::thread s_deferredScanThread;
    static std::thread s_lazyProbeThread;

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

            // Table not meaningfully populated yet — keep waiting without
            // burning attempt budget.
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

    void join_background_threads()
    {
        std::lock_guard<std::mutex> lk(s_bgThreadMtx);
        if (s_deferredScanThread.joinable())
            s_deferredScanThread.join();
        if (s_lazyProbeThread.joinable())
            s_lazyProbeThread.join();
    }

} // namespace EquipHide
