#include "transmog_worker.hpp"
#include "constants.hpp"
#include "item_name_table.hpp"
#include "preset_manager.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"

#include <cdcore/controlled_char.hpp>

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
            // user+0xD8 holds the currently-controlled character's
            // ClientChildOnlyInGameActor. It coincides with user+0xD0
            // (the "primary" slot, always Kliff) when Kliff is the
            // active character, and rotates to Damiane's or Oongka's
            // actor when one of them is controlled. Reading +0xD0
            // unconditionally would land all companion applies on
            // Kliff's actor, which is why the apply pipeline always
            // walks +0xD8.
            auto actor = *reinterpret_cast<uintptr_t *>(user + 0xD8);
            if (actor < 0x10000)
                return 0;

            // Pointer-validity check on the typeEntry slot. The byte at
            // typeEntry+1 is role-based (controlled vs backgrounded)
            // and not stable per-character, so it cannot gate the chain
            // walk; rejecting on it would silently drop companion
            // applies. A readable typeEntry pointer is sufficient
            // structural evidence that the actor is alive.
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

    // Apply ALWAYS targets the currently-controlled character. This
    // helper re-reads the live character via the Core resolver and,
    // when PresetManager has a different active character cached,
    // switches to the live one and rebuilds slot_mappings from its
    // preset. Holds no __try block so std::string usage is fine; called
    // from run_debounced_apply (which cannot mix __try with C++ objects).
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
        // the character whose equip event triggered this apply. Fall
        // back to resolve_player_component() only when player_a1 is
        // empty or unreadable (post-reload / loading screen). The
        // SEH-guarded actor deref below catches a stale wrapper after
        // world reload by dropping the apply.
        //
        // Apply ALWAYS targets the currently-controlled character.
        // The UI's "active character" is just a view into the JSON
        // preset list -- a user can pick Oongka in the overlay while
        // controlling Damiane, but we still want Damiane to end up
        // wearing Damiane's preset. The helper below syncs
        // PresetManager + slot_mappings to the live character. Lives
        // outside this __try-containing function so std::string
        // destructors do not trip MSVC C2712.
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

    // SEH-isolated walk of WorldSystem -> ActorManager -> UserActor.
    // The UserActor pointer is stable for the lifetime of a save: the
    // singleton itself is never swapped when the player changes which
    // body they control (only user+0xD8 rotates between actor slots).
    // A change of the UserActor pointer is therefore a reliable signal
    // for "new world loaded" -- distinct from an in-session character
    // swap, which does NOT reallocate this singleton.
    //
    // Returns 0 on any fault or on an unresolved chain; callers treat 0
    // as "chain not yet ready" and defer the save-load branch.
    static std::uintptr_t read_user_actor_ptr_seh() noexcept
    {
        const auto wsBase =
            world_system_ptr().load(std::memory_order_acquire);
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
            return user;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    /** @brief Settle window for the character-swap detector.
     *  @details During world load the engine rotates `user+0xD8` through
     *           the party members as it wires each actor (e.g. Kliff ->
     *           Oongka -> Damiane on a Damiane save). Each rotation is a
     *           genuine engine state, so the resolver reports the
     *           transient identities; firing the swap immediately would
     *           apply the wrong character's preset and incur ~3x wasted
     *           apply work plus a brief visual flicker. Requiring the new
     *           identity to remain stable across this window before
     *           committing the swap collapses the load-time churn into
     *           a single transition while still propagating real
     *           user-initiated swaps after a 1s delay. */
    static constexpr std::uint64_t k_charSwapSettleMs = 1000;

    static DWORD WINAPI load_detect_thread_fn(LPVOID)
    {
        auto &logger = DMK::Logger::get_instance();
        __int64 prevComp = 0;
        std::uintptr_t prevUser = 0;
        std::string prevCharName;

        // Settle-window state for the swap detector below. pendingCharName
        // is empty when no candidate is in flight; otherwise it holds the
        // candidate name observed since pendingFirstSeenTick. The swap
        // commits only when (now - pendingFirstSeenTick) >= settle window.
        std::string pendingCharName;
        std::uint64_t pendingFirstSeenTick = 0;

        while (!shutdown_requested().load(std::memory_order_relaxed))
        {
            sleep_interruptible(1000);
            if (shutdown_requested().load(std::memory_order_relaxed))
                return 0;

            // --- Save-load invalidation ---
            //
            // The Core controlled-character resolver caches the most
            // recent known identity key. On a save load the previous
            // world's actor pool is freed and reallocated; without
            // invalidation the resolver could keep returning the prior
            // save's character until the chain walk first observes the
            // new actor's identity u32s.
            //
            // Use the UserActor pointer as the save-load signal: the
            // ActorManager rewrites it on world load but leaves it
            // alone during in-session character swaps (only
            // user+0xD8 rotates on swap). Invalidating on every comp
            // change would wipe the cache during normal swaps and add
            // a full 1s tick of latency before the char-swap detector
            // below could confirm the new name.
            //
            // Runs BEFORE the char-swap block so a save-load event
            // routes through the retry loop further down instead of
            // firing a spurious "swap detected" log against a stale
            // cached name.
            {
                const auto curUser = read_user_actor_ptr_seh();
                if (curUser != 0 && curUser != prevUser)
                {
                    if (prevUser != 0)
                    {
                        logger.info(
                            "Load detect: UserActor swapped "
                            "({:#x} -> {:#x}); invalidating controlled-"
                            "char cache for save-load transition",
                            static_cast<uint64_t>(prevUser),
                            static_cast<uint64_t>(curUser));
                        CDCore::invalidate_controlled_character();
                        prevCharName.clear();
                        pendingCharName.clear();
                    }
                    prevUser = curUser;
                }
            }

            // --- Character-swap auto-detect ---
            //
            // Reads the controlled-character name every tick; when it
            // changes AND remains stable for k_charSwapSettleMs, switches
            // the UI preset list to the new character and schedules an
            // apply. The apply path re-walks the WS chain through
            // user+0xD8 so it always resolves the correct per-character
            // wrapper without needing a BatchEquip event.
            //
            // The settle window absorbs load-time wiring churn where the
            // engine briefly rotates user+0xD8 through party members
            // before settling on the save's controlled actor. See the
            // k_charSwapSettleMs declaration above for the rationale.
            {
                const auto liveName = current_controlled_character_name();
                auto &pm = PresetManager::instance();

                // Branch on three states:
                //   1. liveName empty / matches prevCharName -- no
                //      transition; clear any pending candidate so a
                //      back-and-forth (A -> B -> A within settle window)
                //      does not commit a phantom swap to B.
                //   2. liveName differs from pendingCharName -- new
                //      candidate; restart the settle clock.
                //   3. liveName matches pendingCharName -- candidate
                //      held; commit when the elapsed time crosses the
                //      settle threshold.
                if (liveName.empty() || liveName == prevCharName)
                {
                    pendingCharName.clear();
                }
                else if (liveName != pendingCharName)
                {
                    pendingCharName = liveName;
                    pendingFirstSeenTick = GetTickCount64();
                }
                else if (GetTickCount64() - pendingFirstSeenTick >=
                         k_charSwapSettleMs)
                {
                    const std::string oldName = prevCharName.empty()
                                                    ? pm.active_character()
                                                    : prevCharName;
                    prevCharName = liveName;
                    pendingCharName.clear();

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
                // Advance prevComp BEFORE the controlled-char gate so
                // the worker does not re-observe the same change every
                // tick while the world is still loading. The next
                // genuine component change is still detected because
                // prevComp tracks the most recent observed value, not
                // the most recent successfully-applied one.
                logger.info("Load detect: player component changed ({:#x} -> {:#x})",
                            static_cast<uint64_t>(prevComp),
                            static_cast<uint64_t>(comp));
                prevComp = comp;

                // Wipe the controlled-character LKG cache so the gate
                // below decides on a fresh chain walk instead of stale
                // identity from a previous controlled actor. A
                // player_component change always means user+0xD8 now
                // points at a different ClientChildOnlyInGameActor; the
                // LKG identity from the prior actor is no longer
                // applicable, but the resolver's torn-read absorption
                // would otherwise return that stale name on any chain
                // walk that lands on the new wrapper before its slot
                // fields (actor+0x60 slot-index byte used by the
                // companion diff) have been populated. That stale name
                // would route the auto-apply pipeline through
                // PresetManager::set_active_character() with the wrong
                // character, committing the previous character's
                // preset onto the new actor. Invalidating here costs
                // at most one outer-loop tick of latency (the gate
                // defers until the chain walk lands a known key) and
                // is the only correctness-preserving choice on a swap.
                CDCore::invalidate_controlled_character();

                // Safety gate: do not auto-apply until the resolver
                // returns a known character. With LKG invalidated
                // immediately above, liveChar comes back empty until
                // the chain walk on the new wrapper lands a known
                // identity; the retry loop below tolerates that by
                // design (next outer-loop tick re-evaluates).
                const std::string liveChar =
                    current_controlled_character_name();
                if (liveChar.empty())
                {
                    logger.trace(
                        "Load detect: holding auto-apply -- controlled "
                        "char unresolved (waiting for world)");
                    continue;
                }

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
                //
                // Each iteration runs a cheap actor-readiness probe
                // (RealPartTearDown::is_actor_apply_ready) before
                // scheduling a full apply. The probe walks the same
                // container chain that tear_down dereferences but
                // performs no engine calls; on a placeholder wrapper
                // (engine still wiring during world load) the probe
                // returns false at microsecond cost and the iteration
                // skips the full apply, avoiding ~5 SEH-faulted
                // tear_down log lines plus an apply fault per attempt.
                // The 20-attempt budget is preserved for low-end PCs
                // where wiring legitimately takes >60s; the probe just
                // makes the wait silent and cheap.
                //
                // The probe is suppressed on attempt 0 so first-load
                // fast-paths (e.g. the engine warmed before we got
                // scheduled) still fire an apply immediately without
                // an extra delay.
                static constexpr int k_maxAutoApplyAttempts = 20;
                s_lastApplyOk.store(false, std::memory_order_release);

                int notReadyStreak = 0;
                int catalogWaitStreak = 0;
                bool wrapperChanged = false;

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

                    // Mid-retry wrapper-change abort. The engine
                    // sometimes parks user+0xD8 on a placeholder wrapper
                    // for 60+ seconds before deallocating it and
                    // allocating the real character actor at a different
                    // address. Without this check the retry loop would
                    // burn the rest of its budget on the dead placeholder
                    // and only notice the new wrapper when the outer
                    // load-detect tick runs again post-loop. Re-resolving
                    // the component here lets us bail within one
                    // attempt's delay of the swap; the outer loop's next
                    // iteration will see comp != prevComp and start a
                    // fresh retry budget against the new wrapper.
                    {
                        const auto curComp = resolve_player_component();
                        if (curComp > 0x10000 && curComp != comp)
                        {
                            logger.info(
                                "Load detect: wrapper changed mid-retry "
                                "({:#x} -> {:#x}), aborting current budget",
                                static_cast<uint64_t>(comp),
                                static_cast<uint64_t>(curComp));
                            wrapperChanged = true;
                            break;
                        }
                    }

                    // Gate on item-catalog readiness. On hot reload the
                    // game is already mid-session with real equipment
                    // populated, so the "real armor changed" branch in
                    // apply_all_transmog would fire tear_down before the
                    // preset has any resolved item IDs (names still
                    // pending background-thread catalog build). That
                    // strips the character and leaves slots in a state
                    // where the deferred post-resolve re-apply crashes.
                    // Hold the retry budget here until the catalog
                    // publishes; the attempt counter does not advance
                    // during the wait so a slow catalog build does not
                    // exhaust attempts. The outer wrapper-change check
                    // above still runs each tick.
                    if (!ItemNameTable::instance().ready())
                    {
                        if (catalogWaitStreak == 0)
                            logger.debug(
                                "Load detect: catalog not ready -- "
                                "holding auto-apply (attempt {})",
                                attempt + 1);
                        ++catalogWaitStreak;
                        --attempt; // do not consume an attempt slot
                        continue;
                    }
                    if (catalogWaitStreak > 0)
                    {
                        logger.debug(
                            "Load detect: catalog became ready after "
                            "{} deferred ticks", catalogWaitStreak);
                        catalogWaitStreak = 0;
                    }

                    if (attempt > 0 &&
                        !RealPartTearDown::is_actor_apply_ready(
                            reinterpret_cast<void *>(comp)))
                    {
                        // Log once per fault streak so the deferral is
                        // visible in the log without filling it. The
                        // streak resets when a probe finally passes or
                        // when the wrapper changes (next outer-loop
                        // iteration).
                        if (notReadyStreak == 0)
                            logger.debug(
                                "Load detect: actor not ready -- deferring "
                                "apply (attempt {})", attempt + 1);
                        ++notReadyStreak;
                        continue;
                    }

                    if (notReadyStreak > 0)
                    {
                        logger.debug(
                            "Load detect: actor became ready after {} "
                            "deferred attempts", notReadyStreak);
                        notReadyStreak = 0;
                    }

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
                // Suppress the failure warning when we aborted because
                // the wrapper changed -- that path is a planned hand-off
                // to the outer loop, not a failure.
                if (!wrapperChanged &&
                    !s_lastApplyOk.load(std::memory_order_acquire))
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
