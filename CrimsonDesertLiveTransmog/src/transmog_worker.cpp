#include "transmog_worker.hpp"
#include "constants.hpp"
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
    // time after our DLL init runs -- exactly when depends on world load
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

    static void deferred_nametable_scan_fn() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        const auto subTranslator = resolved_addrs().subTranslator;
        if (!subTranslator)
            return;

        using BR = ItemNameTable::BuildResult;

        // Initial grace period -- lets the game finish loading its
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
                // Load display names before dump so the sorted cache
                // built by the dump already contains them.
                {
                    const auto dir = runtime_dir_utf8();
                    if (!dir.empty())
                        ItemNameTable::instance().load_display_names(
                            dir + DISPLAY_NAMES_FILE);
                }
                if (logger.is_enabled(DMK::LogLevel::Trace))
                    ItemNameTable::instance().dump_catalog_tsv();

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
                    "[nametable] deferred scan hit fatal chain error -- "
                    "item catalog unavailable, mod disabled");
                flag_enabled().store(false, std::memory_order_release);
                return;
            }

            // Deferred: sleep and retry indefinitely until the game
            // populates the catalog or shutdown is requested.
            ++attempt;
            if (attempt % 50 == 0)
                logger.warning(
                    "[nametable] still waiting after {} attempts", attempt);

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
            if (ws < 0x10000)
                return 0;
            auto am = *reinterpret_cast<uintptr_t *>(ws + 0x30);
            if (am < 0x10000)
                return 0;
            auto user = *reinterpret_cast<uintptr_t *>(am + 0x28);
            if (user < 0x10000)
                return 0;
            // CE-verified 2026-04-21: `user+0xD8` holds the currently-
            // controlled character's ClientChildOnlyInGameActor. It
            // falls back to Kliff's actor when Kliff is the active
            // character (Kliff's +0xD0 and +0xD8 point to the same
            // actor), and switches to Damiane/Oongka's actor when one
            // of them is controlled. Using +0xD0 instead (legacy
            // offset) would always return Kliff's actor regardless of
            // who is controlled, which is why the apply pipeline
            // previously landed all companion presets on Kliff.
            auto actor = *reinterpret_cast<uintptr_t *>(user + 0xD8);
            if (actor < 0x10000)
                return 0;

            // Verify the chain is walkable -- typeEntry pointer has to
            // be a readable heap address. The old check required
            // `typeByte == 1` (Kliff-specific); CE probing showed
            // Damiane's actor carries typeByte=4 and Oongka's has a
            // different value again, so gating on that byte silently
            // rejected companions. Pointer validity is enough.
            auto te = *reinterpret_cast<uintptr_t *>(actor + 0x88);
            if (te < 0x10000)
                return 0;

            auto comp104 = *reinterpret_cast<uintptr_t *>(actor + 104);
            if (comp104 < 0x10000)
                return 0;
            auto component = *reinterpret_cast<uintptr_t *>(comp104 + 56);
            if (component < 0x10000)
                return 0;

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

    // Set by run_debounced_apply: true if the last apply completed
    // without SEH fault, false on exception. Used by load-detect to
    // stop retrying once a boot-time apply succeeds.
    static std::atomic<bool> s_lastApplyOk{false};

    static std::mutex s_applyCvMtx;
    static std::condition_variable s_applyCv;
    static std::thread s_applyWorker;
    static std::atomic<bool> s_applyWorkerStarted{false};

    // Apply ALWAYS targets the currently-controlled character; this
    // helper re-reads the live char via the WS idx byte and, when
    // PresetManager has a different active character cached, switches
    // to the live one and rebuilds slot_mappings from its preset.
    // No __try inside, so std::string usage is fine. Called from
    // run_debounced_apply (which cannot mix __try with C++ objects).
    static void sync_active_char_to_live() noexcept
    {
        const std::string live = current_controlled_character_name();
        if (live.empty())
            return;
        auto &pm = PresetManager::instance();
        if (live == pm.active_character())
            return;
        pm.set_active_character(live);
        for (auto &m : slot_mappings())
        {
            m.active = false;
            m.targetItemId = 0;
        }
        pm.apply_to_state();
        last_applied_ids().fill(0);
        real_damaged().fill(false);
        last_applied_real_ids().fill(0);
        last_applied_carrier_ids().fill(0);
    }

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

        // Prefer the hook-captured a1 from player_a1() -- it identifies
        // the character whose equip event triggered this apply. The
        // WorldSystem chain resolve_player_component() only ever
        // returns Kliff's component regardless of currently-controlled
        // character (verified via CE probes 2026-04-21), so using it
        // unconditionally applied every companion's preset to Kliff.
        //
        // Fall back to resolve_player_component() only when player_a1
        // is empty or unreadable (post-reload / loading screen). The
        // original concern about stale pointers after world reload is
        // addressed by a SEH-guarded actor deref below -- a freed
        // wrapper faults and we drop the apply, same net behaviour.
        //
        // Apply ALWAYS targets the currently-controlled character.
        // The UI's "active character" is just a view into the JSON
        // preset list -- a user could pick Oongka in the overlay
        // while controlling Damiane, but we still want Damiane to
        // end up wearing Damiane's preset. The helper below syncs
        // PresetManager + slot_mappings to the live character.
        // Lives outside this __try-containing function so
        // std::string destructors don't trip C2712.
        sync_active_char_to_live();
        __int64 a1 = player_a1().load(std::memory_order_acquire);
        if (a1 <= 0x10000)
        {
            a1 = resolve_player_component();
            if (a1 <= 0x10000)
                return;
        }

        // Verify the wrapper still points to a live actor. If the
        // world reloaded after the last hook capture, *(a1+8) may
        // fault or yield garbage -- fall back to the WS chain in
        // that case.
        __try
        {
            const auto actor = *reinterpret_cast<uintptr_t *>(a1 + 8);
            if (actor < 0x10000)
            {
                a1 = resolve_player_component();
                if (a1 <= 0x10000)
                    return;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            a1 = resolve_player_component();
            if (a1 <= 0x10000)
                return;
        }

        // Consume the single-slot index. k_slotCount means "all slots".
        const auto slotIdx =
            pending_slot_index().exchange(k_slotCount,
                                          std::memory_order_acq_rel);

        __try
        {
            if (do_clear)
            {
                clear_all_transmog(a1);
                logger.debug("Transmog cleared (debounced)");
            }
            else if (slotIdx < k_slotCount)
            {
                apply_single_slot_transmog(a1, slotIdx);
                logger.debug("Transmog applied slot={} (debounced)",
                             slotIdx);
            }
            else
            {
                apply_all_transmog(a1);
                logger.debug("Transmog applied (debounced)");
            }
            s_lastApplyOk.store(true, std::memory_order_release);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s_lastApplyOk.store(false, std::memory_order_release);
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
            s_applyCv.wait(lk, []
                           { return shutdown_requested().load(std::memory_order_acquire) ||
                                    s_applyPending.load(std::memory_order_acquire); });
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
                s_applyCv.wait_for(lk, waitFor, [&deadline]
                                   { return shutdown_requested().load(
                                                std::memory_order_acquire) ||
                                            s_applyDeadlineTick.load(
                                                std::memory_order_acquire) != deadline; });
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
        std::string prevCharName;

        while (!shutdown_requested().load(std::memory_order_relaxed))
        {
            sleep_interruptible(1000);
            if (shutdown_requested().load(std::memory_order_relaxed))
                return 0;

            // --- Character-swap auto-detect ---
            //
            // Reads the controlled-character index byte every tick;
            // when it changes, switches the UI preset list to the new
            // character and schedules an apply. The apply path
            // re-walks the WS chain through `user+0xD8` so it always
            // resolves the correct per-character wrapper -- no cache
            // or BatchEquip event required.
            {
                const auto liveName = current_controlled_character_name();
                auto &pm = PresetManager::instance();
                if (!liveName.empty() && liveName != prevCharName)
                {
                    const std::string oldName = prevCharName.empty()
                                                    ? pm.active_character()
                                                    : prevCharName;
                    prevCharName = liveName;

                    if (liveName != pm.active_character())
                    {
                        pm.set_active_character(liveName);
                        for (auto &m : slot_mappings())
                        {
                            m.active = false;
                            m.targetItemId = 0;
                        }
                        pm.apply_to_state();
                        {
                            std::lock_guard<std::mutex> lk(s_applyCvMtx);
                            last_applied_ids().fill(0);
                            real_damaged().fill(false);
                            last_applied_real_ids().fill(0);
                            last_applied_carrier_ids().fill(0);
                        }
                        logger.info("Char swap detected: {} -> {}",
                                    oldName, liveName);
                        if (flag_enabled().load(std::memory_order_relaxed))
                            schedule_transmog_ms(200);
                        pm.save();
                    }
                }
            }

            auto comp = resolve_player_component();

            // Detect change: new component appeared or address changed.
            if (comp > 0x10000 && comp != prevComp)
            {
                // Safety gate: don't auto-apply until the controlled-
                // character idx byte is readable (world fully loaded).
                // run_debounced_apply will sync PresetManager to the
                // live char itself, but running the retry loop with
                // an unresolved live char is pointless and noisy.
                const std::string liveChar =
                    current_controlled_character_name();
                if (liveChar.empty())
                {
                    logger.trace(
                        "Load detect: holding auto-apply -- controlled "
                        "char unresolved (waiting for world)");
                    continue; // Don't advance prevComp; retry next tick.
                }

                logger.info("Load detect: player component changed ({:#x} -> {:#x})",
                            static_cast<uint64_t>(prevComp),
                            static_cast<uint64_t>(comp));
                prevComp = comp;
                player_a1().store(comp, std::memory_order_release);

                // Reset cached apply state so the early-out in
                // apply_all_transmog doesn't suppress the re-apply.
                // The scene graph is fresh after reload -- old fake
                // meshes are gone even though the IDs haven't changed.
                //
                // Held under s_applyCvMtx to prevent racing with an
                // in-flight apply on the worker thread, which reads
                // and writes these same non-atomic arrays.
                {
                    std::lock_guard<std::mutex> lk(s_applyCvMtx);
                    last_applied_ids().fill(0);
                    last_applied_real_ids().fill(0);
                    last_applied_carrier_ids().fill(0);
                    real_damaged().fill(false);
                }

                if (!flag_enabled().load(std::memory_order_relaxed))
                    continue;

                // Retry through the debounce worker. The game's visual
                // state isn't ready immediately after load detect -- the
                // first attempt often faults because the PartDef array
                // and scene graph are still being populated. We retry
                // with exponential backoff up to ~90s total, exiting
                // early once the apply succeeds (no SEH fault).
                //
                // Backoff schedule: 2s, 2s, 3s, 4s, 5s, 5s, 5s, ...
                // This gives fast feedback when the game loads quickly
                // but doesn't burn CPU during longer load screens.
                static constexpr int k_maxAutoApplyAttempts = 20;
                s_lastApplyOk.store(false, std::memory_order_release);

                for (int attempt = 0; attempt < k_maxAutoApplyAttempts;
                     ++attempt)
                {
                    const int delayMs =
                        (attempt < 2) ? 2000 : (attempt < 3) ? 3000
                                           : (attempt < 4)   ? 4000
                                                             : 5000;
                    sleep_interruptible(delayMs);
                    if (shutdown_requested().load(std::memory_order_relaxed))
                        break;
                    schedule_transmog_ms(0);
                    logger.debug("Load detect: scheduled apply (attempt {})",
                                 attempt + 1);

                    // Give the worker time to finish, then check result.
                    sleep_interruptible(500);
                    if (s_lastApplyOk.load(std::memory_order_acquire))
                    {
                        logger.info(
                            "Load detect: auto-apply succeeded on attempt {}",
                            attempt + 1);
                        break;
                    }
                }
                if (!s_lastApplyOk.load(std::memory_order_acquire))
                    logger.warning(
                        "Load detect: auto-apply failed after {} attempts",
                        k_maxAutoApplyAttempts);
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
            // Polls via sleep_interruptible(1000), auto-apply retries up
            // to 20x with backoff. All sleeps check shutdown_requested
            // every 100ms, so drain completes well under 1s.
            WaitForSingleObject(s_loadDetectThread, 15000);
            CloseHandle(s_loadDetectThread);
            s_loadDetectThread = nullptr;
        }
    }

} // namespace Transmog
