#include "equip_hide.hpp"
#include "aob_resolver.hpp"
#include "armor_injection.hpp"
#include "background_threads.hpp"
#include "bald_fix.hpp"
#include "cascade_suppress.hpp"
#include "categories.hpp"
#include "constants.hpp"
#include "gliding_fix.hpp"
#include "indexed_string_table.hpp"
#include "input_handler.hpp"
#include "player_detection.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace EquipHide
{
    // Indexed by truncated part hash; R8B gate-skip lock prevents PartInOut
    // from re-running the transition dispatch after the first vis=2 frame.
    static uint8_t s_hideLocked[0x10000]{};

    // Brief guard window after any hotkey toggle.  Prevents cascade from
    // other armor slots (shield/helm) from briefly hiding legs.
    static std::atomic<int> s_flushGuard{0};

    static constexpr CategoryMask k_cascadeBodyMask =
        category_bit(Category::Legs) |
        category_bit(Category::Gloves) |
        category_bit(Category::Boots);

    // --- Config ---
    static void load_config()
    {
        DMK::Config::register_log_level(
            "General", "LogLevel", "INFO");

        DMK::Config::register_atomic<bool>(
            "General", "ForceShow", "Force Show", flag_force_show(), false);
        DMK::Config::register_atomic<bool>(
            "General", "BaldFix", "Bald Fix", flag_bald_fix(), true);
        DMK::Config::register_atomic<bool>(
            "General", "GlidingFix", "Gliding Fix", flag_gliding_fix(), true);
        DMK::Config::register_atomic<bool>(
            "General", "IndependentToggle", "Independent Toggle",
            flag_independent_toggle(), false);
        DMK::Config::register_atomic<bool>(
            "General", "CascadeFix", "Cascade Fix", flag_cascade_fix(), false);

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto cat = static_cast<Category>(i);
            const std::string section{category_section(cat)};

            const bool active = (cat == Category::Shields ||
                                 cat == Category::Helm ||
                                 cat == Category::Mask);

            DMK::Config::register_bool(section, "Enabled", section + " Enabled", [i](bool val)
                                       { category_states()[i].enabled.store(val, std::memory_order_relaxed); }, active);

            DMK::Config::register_bool(section, "DefaultHidden", section + " Default Hidden", [i](bool val)
                                       { category_states()[i].hidden.store(val, std::memory_order_relaxed); }, active);

            DMK::Config::register_string(section, "Parts", section + " Parts", [cat](const std::string &val)
                                         { register_parts(cat, val); }, default_parts_string(cat));

            // Per-character Parts overrides: [Section:Kliff], [Section:Damiane],
            // [Section:Oongka]. Empty value (section missing) inherits from base.
            for (std::size_t charIdx = 0; charIdx < kCharIdxCount; ++charIdx)
            {
                const std::string charName{character_name_for_idx(charIdx)};
                const std::string charSection = section + ":" + charName;
                const std::string logLabel = section + " Parts (" + charName + ")";
                DMK::Config::register_string(
                    charSection, "Parts", logLabel,
                    [cat, charIdx](const std::string &val)
                    { set_per_char_parts(cat, charIdx, val); },
                    "");
            }
        }

        // Auto-reload toggle. Off-by-default would force a relaunch
        // for every INI tweak; on-by-default keeps the iteration loop
        // tight. Setters invoked from the watcher thread are idempotent.
        static std::atomic<bool> s_autoReload{true};
        DMK::Config::register_atomic<bool>(
            "General", "AutoReloadConfig", "Auto-Reload Config",
            s_autoReload, true);

        // Hotkey bindings (Toggle/Show/Hide per category + ShowAll/HideAll)
        // are registered via DMK::Config::register_press_combo, which fuses
        // the INI key registration with the InputManager press registration.
        // Must precede Config::load() so the press_combo INI keys land in
        // the same load pass as the rest of the config items above.
        register_hotkeys();

        DMK::Config::load(INI_FILE);
        DMK::Config::log_all();

        build_part_lookup();
        update_hidden_mask();

        if (s_autoReload.load(std::memory_order_relaxed))
        {
            // Per-setter callbacks refresh the visibility atomics inline.
            // The reload tail re-derives cached hidden-state masks and
            // hands the actual vis-byte commit to the game thread via
            // needs_direct_write, the same primitive that drives
            // character-swap re-application. Doing the writes from the
            // watcher thread directly contends with the game thread's
            // vis_write_mutex (try_lock bails silently) and skips the
            // s_hideLocked clear that the mid-hook performs alongside
            // its writes, so the next mid-hook frame would see stale
            // locks and refuse to re-transition. The Parts= setter
            // populates the inactive lookup buffer but does not flip
            // s_activeMap; rebuild_part_lookup() rebuilds from the
            // stored per-category strings (honouring per-character
            // overrides) and atomically publishes the new buffer so
            // the mid-hook and direct-write paths see edited part
            // lists on the next tick.
            const auto status = DMK::Config::enable_auto_reload(
                std::chrono::milliseconds{250},
                [](bool content_changed)
                {
                    auto &logger = DMK::Logger::get_instance();
                    if (!content_changed)
                    {
                        logger.info("INI auto-reload: skipped (no content delta)");
                        return;
                    }

                    rebuild_part_lookup();
                    update_hidden_mask();

                    auto &ps = player_state();
                    for (int i = 0; i < k_maxProtagonists; ++i)
                        ps.armorInjected[i].store(
                            false, std::memory_order_relaxed);

                    needs_direct_write().store(
                        true, std::memory_order_release);

                    logger.info("INI auto-reload: setters applied, "
                                "visibility scheduled");
                });
            if (status != DMK::Config::AutoReloadStatus::Started &&
                status != DMK::Config::AutoReloadStatus::AlreadyRunning)
            {
                DMK::Logger::get_instance().warning(
                    "INI auto-reload could not start (status enum {})",
                    static_cast<int>(status));
            }
        }

        {
            auto &logger = DMK::Logger::get_instance();
            if (logger.get_log_level() <= DMK::LogLevel::Trace)
            {
                const auto &states = category_states();
                const auto &partMap = get_part_map();

                for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                {
                    const auto cat = static_cast<Category>(i);
                    const auto bit = category_bit(cat);
                    int count = 0;
                    for (const auto &[hash, mask] : partMap)
                    {
                        if (mask & bit)
                            ++count;
                    }

                    const bool enabled = states[i].enabled.load(std::memory_order_relaxed);
                    const bool hidden = states[i].hidden.load(std::memory_order_relaxed);

                    if (!enabled)
                        logger.trace("Category {}: disabled ({} parts registered)",
                                     category_section(cat), count);
                    else
                        logger.trace("Category {}: enabled, default={} ({} parts)",
                                     category_section(cat),
                                     hidden ? "hidden" : "visible", count);
                }
            }
        }
    }

    // --- Mid-hook callback ---

    static void on_vis_check_impl(SafetyHookContext &ctx)
    {
        if (needs_direct_write().load(std::memory_order_relaxed) &&
            needs_direct_write().exchange(false, std::memory_order_relaxed))
        {
            // Clear stale locks on vis ctrl change (save load, zone
            // transition) so chest gets a fresh first-frame transition.
            if (flag_cascade_fix().load(std::memory_order_relaxed))
                std::memset(s_hideLocked, 0, sizeof(s_hideLocked));

            inject_armor_entries();
            apply_direct_vis_write();
        }

        // Equipment change re-sync with debounce (CascadeFix only).
        if (flag_cascade_fix().load(std::memory_order_relaxed))
        {
            static int64_t s_equipPending = 0;
            if (consume_equip_change())
                s_equipPending = steady_ms();

            if (s_equipPending > 0)
            {
                static uint32_t s_debTick = 0;
                if ((++s_debTick & 0x3F) == 0 &&
                    (steady_ms() - s_equipPending) > 500)
                {
                    for (int i = 0; i < 0x10000; ++i)
                        if (s_hideLocked[i] == 1)
                            s_hideLocked[i] = 2;
                    s_equipPending = 0;
                }
            }
        }

        if (deferred_scan_pending().load(std::memory_order_relaxed))
            launch_deferred_scan();

        if (lazy_probe_pending().load(std::memory_order_relaxed))
        {
            launch_lazy_probe();
            static std::atomic<uint32_t> s_probeCounter{0};
            if ((s_probeCounter.fetch_add(1, std::memory_order_relaxed) & 0xFFF) == 0)
            {
                const auto now = steady_ms();
                auto prev = lazy_probe_signal().load(std::memory_order_relaxed);
                if (prev == 0 || (now - prev) >= k_lazyProbeIntervalMs)
                    lazy_probe_signal().store(now, std::memory_order_relaxed);
            }
        }

        auto r10 = ctx.r10;
        if (r10 < 0x10000)
            return;

        auto partHash = *reinterpret_cast<const uint32_t *>(r10);

        if (!needs_classification(partHash))
            return;

        const auto mask = classify_part(partHash);
        if (mask == 0)
            return;

        // PartInOut struct pointer is in RAX (loaded from [rbp+5F] before hook).
        auto partInOut = ctx.rax;
        if (partInOut < 0x10000)
            return;

        const bool cascadeOn = flag_cascade_fix().load(std::memory_order_relaxed);
        const auto hashIdx = static_cast<uint16_t>(partHash);
        const bool isChest = (mask & category_bit(Category::Chest)) != 0;

        // Protect legs from cascade during the brief window after a
        // hotkey toggle (e.g. shield show/hide triggers a re-evaluation
        // that would otherwise flash the pants).
        if (cascadeOn)
        {
            auto guard = s_flushGuard.load(std::memory_order_relaxed);
            if (guard > 0 &&
                s_flushGuard.compare_exchange_strong(
                    guard, guard - 1, std::memory_order_relaxed))
            {
                if ((mask & k_cascadeBodyMask) != 0 &&
                    !is_any_category_hidden(mask) &&
                    is_category_hidden(Category::Chest))
                {
                    ctx.r8 = 1;
                    return;
                }
            }
        }

        // Chest lock state machine (BEFORE player filter):
        //   0 = unlocked -- first frame, let gate pass
        //   1 = locked   -- R8B=1, skip gate
        //   2 = re-equip -- force vis=0 (In, recreate scene nodes)
        if (cascadeOn && isChest &&
            s_hideLocked[hashIdx] &&
            is_any_category_hidden(mask))
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(partInOut + 0x1C);
            if (s_hideLocked[hashIdx] == 2)
            {
                *visPtr = 0;
                s_hideLocked[hashIdx] = 0;
                return;
            }
            *visPtr = 2;
            ctx.r8 = 1;
            return;
        }

        auto a1 = *reinterpret_cast<uintptr_t *>(ctx.rbp + 0x4F);
        if (!check_player_filter(a1))
            return;

        // Per-character override resolution. The cascade and chest-lock
        // paths above gate engine-wide state and intentionally use the
        // GLOBAL classify_part / is_any_category_hidden masks; this block
        // keeps the actual vis=2 write per-character so an INI override
        // that excludes a hash for one protagonist does not get hidden
        // by the mid-hook on that protagonist's frames.
        //
        // The vis-ctrl scan is O(n) where n is at most k_maxProtagonists
        // (3 after the phantom-filter ship), so the added cost is a
        // handful of relaxed atomic loads + one extra flat-table lookup
        // per call. Relaxed ordering is sufficient: the resolve poll
        // thread publishes consistent (visCtrls[i], visCharIdx[i])
        // pairs on each pass, and a torn read produces at worst one
        // stale per-char idx for a single frame, well within the
        // existing swap-detect timing tolerance.
        int charIdx = -1;
        {
            auto &ps = player_state();
            const auto vcCount = ps.count.load(std::memory_order_relaxed);
            for (int i = 0; i < vcCount; ++i)
            {
                if (ps.visCtrls[i].load(std::memory_order_relaxed) == a1)
                {
                    charIdx = ps.visCharIdx[i].load(std::memory_order_relaxed);
                    break;
                }
            }
        }

        // Refine the hide decision against the per-character part map.
        // charIdx == -1 (untracked actor) preserves legacy behaviour by
        // falling back to the global mask computed earlier.
        CategoryMask charMask = mask;
        if (charIdx >= 0 && charIdx < static_cast<int>(kCharIdxCount))
        {
            charMask = classify_part_for(partHash, charIdx);
            if (charMask == 0)
                return; // hash excluded from this character's effective Parts
        }

        if (is_any_category_hidden_for(charMask, charIdx))
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(partInOut + 0x1C);
            *visPtr = 2;
            if (cascadeOn && isChest)
            {
                if (s_hideLocked[hashIdx])
                    ctx.r8 = 1; // gate skip on locked frames
                else
                    s_hideLocked[hashIdx] = 1;
            }
        }
        else
        {
            if (cascadeOn && isChest)
                s_hideLocked[hashIdx] = 0;

            if (flag_force_show().load(std::memory_order_relaxed))
            {
                auto *visPtr = reinterpret_cast<uint8_t *>(partInOut + 0x1C);
                *visPtr = 0;
            }
        }
    }

    /* SEH wrapper: separate function because MSVC SEH cannot coexist with
       C++ destructors in the same frame. Swallows faults if the mod is
       outdated and register layout has changed -- don't crash the game. */
    static int seh_filter(unsigned int /*code*/) { return EXCEPTION_EXECUTE_HANDLER; }

    static void on_vis_check(SafetyHookContext &ctx)
    {
        __try
        {
            on_vis_check_impl(ctx);
        }
        __except (seh_filter(GetExceptionCode()))
        {
        }
    }

    // --- Public interface ---
    bool init()
    {
        auto &logger = DMK::Logger::get_instance();

        if (!DMK::Memory::init_cache())
            logger.warning("Memory cache init failed -- pointer reads may be slower");

        auto &addrs = resolved_addrs();

        addrs.worldSystem = resolve_address(
            k_worldSystemCandidates, std::size(k_worldSystemCandidates),
            "WorldSystem");

        // Publish the WorldSystem holder to the Core controlled-character
        // resolver. Core walks WorldSystem -> ActorManager -> UserActor ->
        // controlled_actor(+0xD8) -> identity u32s at +0xDC and +0xEC.
        // Idempotent across consumers: when CrimsonDesertLiveTransmog is
        // loaded alongside, both publish the same address and the later
        // writer wins (the intended behaviour after a re-resolve).
        CDCore::set_world_system_holder(addrs.worldSystem);

        addrs.childActorVtbl = resolve_address(
            k_childActorVtblCandidates, std::size(k_childActorVtblCandidates),
            "ChildActorVtbl");

        addrs.mapLookup = resolve_address(
            k_mapLookupCandidates, std::size(k_mapLookupCandidates),
            "MapLookup");

        addrs.mapInsert = resolve_address(
            k_mapInsertCandidates, std::size(k_mapInsertCandidates),
            "MapInsert");

        // Resolve IndexedStringA global from MapLookup: mov rax, [rip+disp] at +20
        if (addrs.mapLookup)
        {
            auto resolved = DMK::Scanner::find_and_resolve_rip_relative(
                reinterpret_cast<const std::byte *>(addrs.mapLookup + 20),
                7, DMK::Scanner::PREFIX_MOV_RAX_RIP, 7);
            if (resolved)
            {
                addrs.indexedStringGlobal = *resolved;
                logger.info("IndexedStringA global resolved at 0x{:X}",
                            addrs.indexedStringGlobal);
            }
            else
            {
                logger.warning("IndexedStringA global: expected MOV RAX,[rip+disp] at "
                               "MapLookup+20, armor injection disabled");
            }
        }

        if (!addrs.worldSystem || !addrs.childActorVtbl)
        {
            flag_fallback_mode().store(true, std::memory_order_relaxed);
            logger.info("Player identification: type byte fallback (global chain AOB unavailable)");
        }
        else
        {
            logger.info("Player identification: global pointer chain");
        }

        if (!addrs.mapLookup)
        {
            logger.warning("MapLookup not resolved, cannot scan IndexedStringA table");
        }

        load_config();
        original_vis_map().reserve(get_part_map().size());

        // Initial IndexedStringA scan + deferred-launch decision.
        // Mirrors LiveTransmog's transmog_worker pattern: commit the sync
        // attempt so the mod is immediately functional, then launch the
        // deferred worker unless the table is already fully resolved on
        // the first try. Convergence in the worker is stability-based
        // (LT's structural-ready analog), not a coverage percentage.
        if (addrs.mapLookup)
        {
            auto runtimeHashes = scan_indexed_string_table(addrs.mapLookup);
            const auto initialResolved = runtimeHashes.size();
            const auto totalExpected = total_part_count();

            if (initialResolved > 0)
                set_runtime_hashes(std::move(runtimeHashes));

            const bool fullyResolved = totalExpected > 0 &&
                                       initialResolved == totalExpected;
            if (!fullyResolved)
            {
                logger.info(
                    "IndexedStringA scan: {}/{} entries at init, "
                    "starting deferred scan thread (stability-check mode)",
                    initialResolved, totalExpected);
                deferred_scan_pending().store(true, std::memory_order_relaxed);
                launch_deferred_scan();
            }
            else
            {
                logger.info(
                    "IndexedStringA scan: {}/{} entries at init "
                    "(fully resolved, no deferred retry)",
                    initialResolved, totalExpected);
            }

            // Rebuild the part lookup against whatever subset got committed
            // so the active map reflects the partial-or-full hash set.
            // The deferred worker will rebuild again when it converges.
            if (initialResolved > 0)
                rebuild_part_lookup();
        }

        // Mid-body scan with the shared cascade resolver. The
        // prologue-fallback variant survives dev hot-reload: when a
        // prior Logic-DLL load left a SafetyHook detour-jump in place
        // at this site, every original-bytes candidate fails on rescan,
        // and the resolver retries each Direct candidate with the first
        // five byte-tokens replaced by the near-JMP signature
        // E9 ?? ?? ?? ??. The candidate's disp_offset is preserved, so
        // the returned address still lands on the original cmp instr.
        auto hookHit = DMK::Scanner::resolve_cascade_with_prologue_fallback(
            std::span<const AddrCandidate>{k_hookSiteCandidates,
                                           std::size(k_hookSiteCandidates)},
            "EquipVisCheckHookSite");

        if (!hookHit.has_value())
        {
            logger.error("No AOB pattern matched. The mod may be outdated for this game version.");
            return false;
        }

        const auto hookAddr = hookHit->address;

        auto &hookMgr = DMK::HookManager::get_instance();
        auto hookResult = hookMgr.create_mid_hook("EquipVisCheck", hookAddr, on_vis_check);

        if (!hookResult.has_value())
        {
            logger.error("Hook creation failed at 0x{:X}: {}",
                         hookAddr, DetourModKit::Hook::error_to_string(hookResult.error()));
            return false;
        }

        logger.info("Hook installed via pattern '{}' at 0x{:X}",
                    hookHit->winning_name, hookAddr);

        // Prevents hidden parts from flashing during state transitions (gliding exit).
        if (flag_gliding_fix().load(std::memory_order_relaxed))
        {
            auto partAddShowAddr = resolve_address(
                k_partAddShowCandidates, std::size(k_partAddShowCandidates),
                "PartAddShow");

            if (partAddShowAddr)
            {
                PartAddShowFn trampoline = nullptr;
                auto result = hookMgr.create_inline_hook(
                    "PartAddShow", partAddShowAddr,
                    reinterpret_cast<void *>(on_part_add_show),
                    reinterpret_cast<void **>(&trampoline));

                if (result.has_value())
                {
                    set_part_add_show_trampoline(trampoline);
                    logger.info("PartAddShow inline hook installed at 0x{:X}",
                                partAddShowAddr);
                }
                else
                    logger.warning("PartAddShow hook failed: {} -- gliding flash fix disabled",
                                   DetourModKit::Hook::error_to_string(result.error()));
            }
            else
            {
                logger.warning("PartAddShow AOB scan failed -- gliding flash fix disabled");
            }
        }

        // Prevents baldness when hiding helmets/cloaks.  The hook temporarily
        // sets bit 19 in item+0x70 bitmasks per-call so PostfixEval sees
        // inactive priority and hair-hiding rules don't match.  Player vs
        // NPC invocations are distinguished by a call-graph landmark: NPC
        // PostfixEval calls always traverse a specific caller whose return
        // address we resolve via AOB (npcPfeReturnAddr); the hook scans
        // its own stack window for that landmark and rejects NPC calls.
        if (flag_bald_fix().load(std::memory_order_relaxed))
        {
            auto postfixEvalAddr = resolve_address(
                k_postfixEvalCandidates, std::size(k_postfixEvalCandidates),
                "PostfixEval");

            auto npcPfeLandmark = resolve_address(
                k_npcPfeReturnAddrCandidates,
                std::size(k_npcPfeReturnAddrCandidates),
                "NpcPfeReturnAddr");
            // Landmark sits 12 bytes past the match start (first byte of the
            // `mov rbx, [rsp+4A0]` right after the call), which is the real
            // return address on an NPC stack frame.
            if (npcPfeLandmark)
                addrs.npcPfeReturnAddr = npcPfeLandmark + 12;

            if (postfixEvalAddr && addrs.npcPfeReturnAddr)
            {
                PostfixEvalFn trampoline = nullptr;
                auto result = hookMgr.create_inline_hook(
                    "PostfixEval", postfixEvalAddr,
                    reinterpret_cast<void *>(on_postfix_eval),
                    reinterpret_cast<void **>(&trampoline));

                if (result.has_value())
                {
                    set_postfix_eval_trampoline(trampoline);
                    logger.info("PostfixEval inline hook installed at 0x{:X}, "
                                "npc-caller landmark 0x{:X} -- bald fix active",
                                postfixEvalAddr, addrs.npcPfeReturnAddr);
                }
                else
                    logger.warning("PostfixEval hook failed: {} -- bald fix disabled",
                                   DetourModKit::Hook::error_to_string(result.error()));
            }
            else if (!postfixEvalAddr)
            {
                logger.warning("PostfixEval AOB scan failed -- bald fix disabled");
            }
            else
            {
                logger.warning("NpcPfeReturnAddr AOB scan failed -- bald fix disabled "
                               "(refusing to run without the call-graph filter)");
            }
        }
        else
        {
            logger.info("BaldFix disabled in config -- hair-hiding rules will apply normally");
        }

        // Equipment change detection for CascadeFix re-sync.
        // Clears the R8B gate-skip locks when chest armor is changed
        // so the new gear gets a fresh Out transition.
        if (flag_cascade_fix().load(std::memory_order_relaxed))
        {
            auto vecAddr = resolve_address(
                k_visualEquipChangeCandidates,
                std::size(k_visualEquipChangeCandidates),
                "VisualEquipChange");

            if (vecAddr)
            {
                VisualEquipChangeFn trampoline = nullptr;
                auto result = hookMgr.create_inline_hook(
                    "VisualEquipChange", vecAddr,
                    reinterpret_cast<void *>(on_visual_equip_change),
                    reinterpret_cast<void **>(&trampoline));

                if (result.has_value())
                {
                    set_visual_equip_change_trampoline(trampoline);
                    logger.info("VisualEquipChange hook installed at 0x{:X}", vecAddr);
                }
                else
                    logger.warning("VisualEquipChange hook failed: {}",
                                   DetourModKit::Hook::error_to_string(result.error()));
            }

            auto vesAddr = resolve_address(
                k_visualEquipSwapCandidates,
                std::size(k_visualEquipSwapCandidates),
                "VisualEquipSwap");

            if (vesAddr)
            {
                VisualEquipSwapFn trampoline = nullptr;
                auto result = hookMgr.create_inline_hook(
                    "VisualEquipSwap", vesAddr,
                    reinterpret_cast<void *>(on_visual_equip_swap),
                    reinterpret_cast<void **>(&trampoline));

                if (result.has_value())
                {
                    set_visual_equip_swap_trampoline(trampoline);
                    logger.info("VisualEquipSwap hook installed at 0x{:X}", vesAddr);
                }
                else
                    logger.warning("VisualEquipSwap hook failed: {}",
                                   DetourModKit::Hook::error_to_string(result.error()));
            }
        }

        // Hotkey bindings were registered in load_config() so the
        // press_combo INI keys could be picked up during Config::load().
        // Now flip InputManager live.
        auto &inputMgr = DMK::InputManager::get_instance();
        inputMgr.start();

        // Drives resolve_player_vis_ctrls on a fixed cadence so cold
        // load and in-session character swaps are detected without
        // depending on the EquipVisCheck hook's event stream. See
        // background_threads.cpp for the thread body.
        launch_resolve_poll();

        if (deferred_scan_pending().load(std::memory_order_relaxed))
            logger.info("Hooks installed, part hashes pending (deferred scan active)");
        else
            logger.info("Equip hide fully initialized ({} parts resolved)",
                        total_part_count());
        return true;
    }

    void arm_flush_guard() noexcept
    {
        if (flag_cascade_fix().load(std::memory_order_relaxed))
            s_flushGuard.store(500, std::memory_order_relaxed);
    }

    void shutdown()
    {
        auto &logger = DMK::Logger::get_instance();
        logger.info("{} shutting down...", MOD_NAME);

        // Per-step bracket logs around each blocking call let any
        // future shutdown stall be pinpointed to the exact step
        // (worker join, vis-byte cleanup, DMK teardown, etc.). The
        // teardown path crosses several mutexes and the SafetyHook
        // trampoline drain window, so a silent hang would otherwise
        // be undiagnosable from the user's log alone. Logger::flush()
        // between steps drains the async queue so a hang inside a
        // step still surfaces every line the prior step emitted.
        logger.info("{} shutdown: step 1 disable_auto_reload", MOD_NAME);
        // Disable the INI watcher up front so an in-flight save event
        // cannot fire setters while we are tearing state down.
        DMK::Config::disable_auto_reload();
        logger.flush();

        logger.info("{} shutdown: step 2 signal stop", MOD_NAME);
        shutdown_requested().store(true, std::memory_order_relaxed);
        lazy_probe_pending().store(false, std::memory_order_relaxed);
        logger.flush();

        logger.info("{} shutdown: step 3 join workers", MOD_NAME);
        // Drain workers before the DMK teardown removes the hooks they
        // call into. shutdown_requested is the cooperative stop signal
        // each StoppableWorker body polls; joining them first guarantees
        // no worker is mid-call into a SafetyHook trampoline when the
        // trampoline pages are unmapped.
        join_background_threads();
        logger.flush();

        logger.info("{} shutdown: step 4 cleanup vis bytes", MOD_NAME);
        // Restore visibility bytes while the hooks are still installed
        // and the game's part registry is reachable. The vis-byte
        // cleanup walks per-actor arrays and writes back the original
        // bytes recorded at first hide; running it after DMK_Shutdown
        // would race the loader unmapping the Logic-DLL pages backing
        // the cleanup function itself.
        cleanup_vis_bytes();
        logger.flush();

        logger.info("{} shutdown: step 5 DMK_Shutdown", MOD_NAME);
        // Full DMK teardown: removes every managed hook (EquipVisCheck,
        // PartAddShow, PostfixEval, VisualEquipChange, VisualEquipSwap),
        // stops and clears the InputManager poller along with its
        // registered bindings, stops the ConfigWatcher, and clears the
        // Config registered-items list. Idempotent and safe to re-init
        // from on the next Logic-DLL load. Each detour body snapshots
        // its trampoline pointer at entry and bails to a benign default
        // if the snapshot is null, defending the brief drain window
        // between hook removal and DLL unmap.
        DMK_Shutdown();
        logger.flush();

        logger.info("{} shutdown: step 6 clear hotkey guards", MOD_NAME);
        clear_hotkey_guards();
        logger.info("{} shutdown complete", MOD_NAME);
        logger.flush();
    }

} // namespace EquipHide
