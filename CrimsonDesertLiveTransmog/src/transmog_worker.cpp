#include "transmog_worker.hpp"
#include "item_name_table.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Transmog
{
    // --- Deferred item-name catalog scan ---
    //
    // The game populates the iteminfo global (`qword_145CEF370`) some
    // time after our DLL init runs — exactly when depends on world load
    // order, so we can't reliably build the catalog in init(). Mirror
    // EquipHide's pattern: a single background thread that sleeps an
    // initial grace period, then retries `ItemNameTable::build` until
    // `BuildResult::Ok` or the attempt budget is exhausted.
    //
    // Once the build lands, the thread calls PresetManager to re-resolve
    // every stored itemName against the freshly populated catalog and
    // re-applies the active preset so the visual snaps into place.
    static std::mutex s_nametableThreadMtx;
    static std::thread s_nametableScanThread;
    static std::atomic<bool> s_nametableScanLaunched{false};

    static constexpr int k_nametableInitialDelayMs = 8000;
    static constexpr int k_nametableRetryMs = 2000;
    static constexpr int k_nametableMaxAttempts = 15;

    static void deferred_nametable_scan_fn() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        const auto subTranslator = resolved_addrs().subTranslator;
        if (!subTranslator)
            return;

        using BR = ItemNameTable::BuildResult;

        // Initial grace period — lets the game finish loading its
        // iteminfo container before we start polling.
        for (int slept = 0; slept < k_nametableInitialDelayMs; slept += 250)
        {
            if (shutdown_requested().load(std::memory_order_relaxed))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        int attempt = 0;
        for (;;)
        {
            if (shutdown_requested().load(std::memory_order_relaxed))
                return;

            const auto result = ItemNameTable::instance().build(subTranslator);
            if (result == BR::Ok)
            {
                const auto size = ItemNameTable::instance().size();
                logger.info(
                    "[nametable] deferred scan succeeded on attempt {} "
                    "({} entries)",
                    attempt + 1, size);

                // Re-resolve all loaded preset slots against the now-
                // populated catalog and re-apply the active preset so
                // the live slot_mappings picks up any drift corrections.
                auto &pm = PresetManager::instance();
                pm.reresolve_all_names();
                pm.apply_to_state();

                // Push the corrected state through the apply pipeline
                // so the visible transmog reflects whatever the deferred
                // resolution just fixed.
                manual_apply();
                return;
            }
            if (result == BR::Fatal)
            {
                logger.error(
                    "[nametable] deferred scan hit fatal chain error — "
                    "item catalog unavailable, mod disabled");
                flag_enabled().store(false, std::memory_order_release);
                return;
            }

            // Deferred: sleep and retry.
            ++attempt;
            if (attempt >= k_nametableMaxAttempts)
            {
                logger.error(
                    "[nametable] deferred scan exhausted {} attempts — "
                    "item catalog unavailable, mod disabled",
                    k_nametableMaxAttempts);
                flag_enabled().store(false, std::memory_order_release);
                return;
            }

            for (int slept = 0; slept < k_nametableRetryMs; slept += 250)
            {
                if (shutdown_requested().load(std::memory_order_relaxed))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
    }

    void launch_deferred_nametable_scan() noexcept
    {
        if (s_nametableScanLaunched.exchange(true, std::memory_order_acq_rel))
            return;

        std::lock_guard<std::mutex> lk(s_nametableThreadMtx);
        s_nametableScanThread = std::thread(deferred_nametable_scan_fn);
    }

    void join_deferred_nametable_scan()
    {
        std::lock_guard<std::mutex> lk(s_nametableThreadMtx);
        if (s_nametableScanThread.joinable())
            s_nametableScanThread.join();
    }

    // --- Player component resolution ---

    __int64 resolve_player_component() noexcept
    {
        const auto wsBase = world_system_ptr().load(std::memory_order_acquire);
        if (!wsBase)
            return 0;
        __try
        {
            auto ws = *reinterpret_cast<uintptr_t *>(wsBase);
            if (ws < 0x10000) return 0;
            auto am = *reinterpret_cast<uintptr_t *>(ws + 0x30);
            if (am < 0x10000) return 0;
            auto user = *reinterpret_cast<uintptr_t *>(am + 0x28);
            if (user < 0x10000) return 0;
            auto actor = *reinterpret_cast<uintptr_t *>(user + 0xD0);
            if (actor < 0x10000) return 0;

            // Verify it's the player via type byte.
            auto te = *reinterpret_cast<uintptr_t *>(actor + 0x88);
            if (te < 0x10000 || *reinterpret_cast<uint8_t *>(te + 1) != 1)
                return 0;

            auto comp104 = *reinterpret_cast<uintptr_t *>(actor + 104);
            if (comp104 < 0x10000) return 0;
            auto component = *reinterpret_cast<uintptr_t *>(comp104 + 56);
            if (component < 0x10000) return 0;

            return static_cast<__int64>(component);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // --- Debounced apply scheduling ---
    // Rapid inventory swaps fire VEC/BatchEquip hooks multiple times in
    // quick succession. Without coalescing, each trigger spawns its own
    // apply and the visible effect is the player cycling head-to-toe
    // while the auth table is still mid-mutation (SEH faults in
    // tear_down). The debounce collapses bursts into a single apply
    // k_applyDebounceMs after the last trigger.
    static std::atomic<std::uint64_t> s_applyDeadlineTick{0};
    static std::atomic<bool> s_applyPending{false};

    static std::mutex s_applyCvMtx;
    static std::condition_variable s_applyCv;
    static std::thread s_applyWorker;
    static std::atomic<bool> s_applyWorkerStarted{false};

    // One apply/clear pass. Pulled out of the worker body so the SEH
    // frame does not share scope with the condition-variable unique_lock
    // (MSVC C2712 forbids __try with objects requiring unwinding).
    static void run_debounced_apply() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        // Consume the clear flag before resolving a1 so a racing
        // manual_clear call doesn't get lost.
        const bool do_clear =
            clear_pending().exchange(false, std::memory_order_acq_rel);

        // Re-resolve the player pointer from the WorldSystem chain.
        // The captured a1 from the triggering hook is not trusted
        // here — by the time the debounce fires the game may have
        // reloaded the world and the old pointer points into freed
        // memory. resolve_player_component returns 0 if the chain
        // is partially populated (pre-world, loading screen) and
        // we silently drop the apply in that case.
        const __int64 a1 = resolve_player_component();
        if (a1 <= 0x10000)
            return;

        __try
        {
            if (do_clear)
            {
                clear_all_transmog(a1);
                logger.debug("Transmog cleared (debounced)");
            }
            else
            {
                apply_all_transmog(a1);
                logger.debug("Transmog applied (debounced)");
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Expected during load-retry window (game visual state not
            // ready yet). Logged at debug to avoid false-alarm noise.
            logger.debug("Transmog {} exception (debounced)",
                         do_clear ? "clear" : "apply");
        }
    }

    // Persistent debounce worker. Sleeps on a condition variable until
    // schedule_transmog bumps the deadline, then waits out any remaining
    // debounce window before invoking run_debounced_apply.
    static void apply_worker_fn() noexcept
    {
        std::unique_lock<std::mutex> lk(s_applyCvMtx);
        for (;;)
        {
            s_applyCv.wait(lk, [] {
                return shutdown_requested().load(std::memory_order_acquire) ||
                       s_applyPending.load(std::memory_order_acquire);
            });
            if (shutdown_requested().load(std::memory_order_acquire))
                return;

            // Wait until the deadline expires, picking up any later
            // re-schedules as they arrive by looping on wait_for.
            for (;;)
            {
                const std::uint64_t deadline =
                    s_applyDeadlineTick.load(std::memory_order_acquire);
                const std::uint64_t now = GetTickCount64();
                if (now >= deadline)
                    break;

                const auto waitFor =
                    std::chrono::milliseconds(deadline - now);
                s_applyCv.wait_for(lk, waitFor, [&deadline] {
                    return shutdown_requested().load(
                               std::memory_order_acquire) ||
                           s_applyDeadlineTick.load(
                               std::memory_order_acquire) != deadline;
                });
                if (shutdown_requested().load(std::memory_order_acquire))
                    return;
            }

            s_applyPending.store(false, std::memory_order_release);
            lk.unlock();

            run_debounced_apply();

            lk.lock();
        }
    }

    void schedule_transmog_ms(std::uint64_t debounce_ms)
    {
        {
            std::lock_guard<std::mutex> lk(s_applyCvMtx);
            s_applyDeadlineTick.store(
                GetTickCount64() + debounce_ms,
                std::memory_order_release);
            s_applyPending.store(true, std::memory_order_release);
        }
        s_applyCv.notify_all();
    }

    void schedule_transmog(__int64 /*a1*/, std::uint16_t /*targetId*/)
    {
        schedule_transmog_ms(k_applyDebounceMs);
    }

    void ensure_apply_worker_started()
    {
        bool expected = false;
        if (!s_applyWorkerStarted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;
        s_applyWorker = std::thread(apply_worker_fn);
    }

    void stop_apply_worker()
    {
        if (!s_applyWorkerStarted.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard<std::mutex> lk(s_applyCvMtx);
            s_applyPending.store(true, std::memory_order_release);
        }
        s_applyCv.notify_all();
        if (s_applyWorker.joinable())
            s_applyWorker.join();
        s_applyWorkerStarted.store(false, std::memory_order_release);
    }

    // --- Load-detection thread ---

    static HANDLE s_loadDetectThread = nullptr;

    static void sleep_interruptible(int ms)
    {
        for (int i = 0; i < ms / 100; ++i)
        {
            if (shutdown_requested().load(std::memory_order_relaxed))
                return;
            Sleep(100);
        }
    }

    static DWORD WINAPI load_detect_thread_fn(LPVOID)
    {
        auto &logger = DMK::Logger::get_instance();
        __int64 prevComp = 0;

        while (!shutdown_requested().load(std::memory_order_relaxed))
        {
            sleep_interruptible(1000);
            if (shutdown_requested().load(std::memory_order_relaxed))
                return 0;

            auto comp = resolve_player_component();

            // Detect change: new component appeared or address changed.
            if (comp > 0x10000 && comp != prevComp)
            {
                logger.info("Load detect: player component changed ({:#x} -> {:#x})",
                            static_cast<uint64_t>(prevComp),
                            static_cast<uint64_t>(comp));
                prevComp = comp;
                player_a1().store(comp, std::memory_order_release);

                if (!flag_enabled().load(std::memory_order_relaxed))
                    continue;

                // Retry through the debounce worker. The game's visual
                // state isn't ready immediately after load detect — the
                // first attempt often faults. Each schedule fires the
                // worker with deadline=now (immediate), and the 2s
                // sleep gives the game time to finish loading between
                // attempts. Applies are serialized through the single
                // worker thread, avoiding data races.
                for (int attempt = 0; attempt < 5; ++attempt)
                {
                    sleep_interruptible(2000);
                    if (shutdown_requested().load(std::memory_order_relaxed))
                        break;
                    schedule_transmog_ms(0);
                    logger.info("Load detect: scheduled apply (attempt {})",
                                attempt + 1);
                }
            }
            else if (comp > 0x10000)
            {
                prevComp = comp;
            }
        }

        return 0;
    }

    void start_load_detect_thread()
    {
        s_loadDetectThread = CreateThread(
            nullptr, 0, load_detect_thread_fn, nullptr, 0, nullptr);
    }

    void stop_load_detect_thread()
    {
        if (s_loadDetectThread)
        {
            // Polls via sleep_interruptible(1000), retries up to 5x
            // with 2s intervals. Worst case: 1s + 5*2s + margin = 12s.
            WaitForSingleObject(s_loadDetectThread, 15000);
            CloseHandle(s_loadDetectThread);
            s_loadDetectThread = nullptr;
        }
    }

} // namespace Transmog
