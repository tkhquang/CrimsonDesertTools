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

#include <cdcore/anchors.hpp>
#include <cdcore/controlled_char.hpp>

#include <DetourModKit/abi/wheel_host.h>
#include <DetourModKit/config.hpp>
#include <DetourModKit/diagnostics.hpp>
#include <DetourModKit/error.hpp>
#include <DetourModKit/hook.hpp>
#include <DetourModKit/input.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/scan.hpp>
#include <DetourModKit/session.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EquipHide
{
    // Indexed by truncated part hash. The gate-skip lock (the In/Out selector forced to Out) stops PartInOut from a
    // re-run of the transition dispatch after the first vis=2 frame.
    static uint8_t s_hideLocked[0x10000]{};

    // Brief guard window after any hotkey toggle. It stops a cascade from another armor slot (shield or helm) from a
    // brief hide of the legs.
    static std::atomic<int> s_flushGuard{0};

    static constexpr CategoryMask k_cascadeBodyMask =
        category_bit(Category::Legs) | category_bit(Category::Gloves) | category_bit(Category::Boots);

    /**
     * @brief In/Out selector value that makes EquipVisCheck return "no opinion" instead of an alpha to publish.
     * @details The decision function's fourth argument is the In/Out selector and the engine reads only its low
     *          byte, at `test r9b,r9b`, `cmp r9b,1` and `cmp r9b,r11b`. Its caller publishes the returned alpha
     *          only after `vcomiss xmm0,0 / jb`, so a negative return skips the publish entirely. That skip is the
     *          gate the chest-lock state machine wants to close.
     *
     *          With the hidden visibility byte (2) written, the three outcomes are:
     *          - selector 0 (In): 0.0f, published, the part hides. This is the normal hide path.
     *          - selector 1 (Out): 1.0f, published, the part SHOWS. Never force this.
     *          - selector >= 2: -1.0f, not published, the part keeps whatever it already is.
     *
     *          2 is the engine's own no-transition selector: the transition function early-returns on it before it
     *          ever reaches this call, so it cannot collide with a live In or Out pass.
     */
    static constexpr uintptr_t k_selectorSkipGate = 2;

    /**
     * @brief Prologue window searched for the `mov rax, [rip+disp32]` that names the IndexedStringA global.
     * @details The instruction sits early in MapLookup, but the compiler is free to schedule other setup ahead of it,
     *          so the window covers the whole prologue rather than one fixed offset.
     */
    static constexpr std::size_t k_mapLookupPrologueBytes = 0x40;

    // Every hook the mod installs. A HookStack restores newest first, the only safe order for layered hooks on
    // one target. In any other order an older layer's restore clobbers a prologue that a newer layer's live
    // trampoline still chains through. shutdown() clears it while the code pages are still mapped.
    static DMK::hook::HookStack s_hooks;

    /// Latched when a hooked prologue fails the restore proof. The dev loader refuses the unmap on it.
    static std::atomic<bool> s_hookRestoreFailed{false};

    // Binds every INI item and hotkey, loads the file, then arms the auto-reload watcher.
    static void load_config(DMK::Session &session)
    {
        // Section-scoped binders, so each INI section name is written once here instead of heading every call.
        const DMK::config::SectionBinder general = DMK::config::section("General");
        const DMK::config::SectionBinder advanced = DMK::config::section("Advanced");

        general.bind_log_level("LogLevel", "INFO");

        general.bind<bool>("BaldFix", "Bald Fix", flag_bald_fix(), true);
        general.bind<bool>("GlidingFix", "Gliding Fix", flag_gliding_fix(), true);
        general.bind<bool>("IndependentToggle", "Independent Toggle", flag_independent_toggle(), false);
        general.bind<bool>("CascadeFix", "Cascade Fix", flag_cascade_fix(), false);

        // Advanced: rtti_dissect self-heal search radius (bytes, per side) for the manager->userActor offset recovery
        // in CDCore. The default 0x200 leaves about ten times the margin of the largest drift to date. Raise it
        // toward MAX_HEAL_WINDOW only if a game patch shifts the field further. Not for normal users.
        advanced.bind<int>("SelfHealWindow", "Self Heal Window", CDCore::heal_window_setting(), 0x200);

        // Protagonist codename overrides for CDCore's appearance-config classifier. Each codename is a substring search
        // target inside the actor's appearance-config asset path. The defaults match the shipped engine subfolder
        // names. The overrides exist in case a future patch or mod renames a subfolder. The loader ignores empty
        // values.
        general.bind_string(
            "KliffCodename",
            "Kliff Codename",
            [](std::string_view val) { CDCore::set_protagonist_codenames(val, {}, {}); },
            "cd_phm_macduff"
        );
        general.bind_string(
            "DamianeCodename",
            "Damiane Codename",
            [](std::string_view val) { CDCore::set_protagonist_codenames({}, val, {}); },
            "cd_phw_damian"
        );
        general.bind_string(
            "OongkaCodename",
            "Oongka Codename",
            [](std::string_view val) { CDCore::set_protagonist_codenames({}, {}, val); },
            "cd_phm_oongka"
        );

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto cat = static_cast<Category>(i);
            const std::string section{category_section(cat)};
            const DMK::config::SectionBinder category = DMK::config::section(section);

            const bool active = (cat == Category::Shields || cat == Category::Helm || cat == Category::Mask);

            category.bind_bool(
                "Enabled",
                section + " Enabled",
                [i](bool val) { category_states()[i].enabled.store(val, std::memory_order_relaxed); },
                active
            );

            category.bind_bool(
                "DefaultHidden",
                section + " Default Hidden",
                [i](bool val) { category_states()[i].hidden.store(val, std::memory_order_relaxed); },
                active
            );

            category.bind_string(
                "Parts",
                section + " Parts",
                [cat](std::string_view val) { register_parts(cat, std::string{val}); },
                default_parts_string(cat)
            );

            // Per-character Parts overrides: [Section:Kliff], [Section:Damiane], [Section:Oongka]. Empty value (section
            // missing) inherits from base.
            for (std::size_t charIdx = 0; charIdx < k_charIdxCount; ++charIdx)
            {
                const std::string charName{character_name_for_idx(charIdx)};
                const std::string logLabel = section + " Parts (" + charName + ")";
                const DMK::config::SectionBinder perCharacter = DMK::config::section(section + ":" + charName);
                perCharacter.bind_string(
                    "Parts",
                    logLabel,
                    [cat, charIdx](std::string_view val) { set_per_char_parts(cat, charIdx, std::string{val}); },
                    ""
                );
            }
        }

        // Auto-reload toggle. Off-by-default forces a relaunch for every INI tweak. On-by-default keeps the iteration
        // loop tight. Setters invoked from the watcher thread are idempotent.
        static std::atomic<bool> s_autoReload{true};
        general.bind<bool>("AutoReloadConfig", "Auto-Reload Config", s_autoReload, true);

        // config::press_combo registers every hotkey binding (Toggle/Show/Hide per category plus ShowAll/HideAll)
        // and fuses the INI key binding with the input press registration. It must precede the INI load so the
        // press_combo keys land in the same load pass as the config items above. The returned guards go into the
        // Session's input scope, which ~Session clears first and in reverse insertion order.
        register_hotkeys(session.scope());

        session.ini().load(INI_FILE);
        DMK::config::log_all();

        build_part_lookup();
        update_hidden_mask();

        if (s_autoReload.load(std::memory_order_relaxed))
        {
            // Per-setter callbacks refresh the visibility atomics inline. The reload tail re-derives cached
            // hidden-state masks and hands the actual vis-byte commit to the game thread via needs_direct_write, the
            // same primitive that drives character-swap re-application. A direct write from the watcher thread
            // contends with the game thread's vis_write_mutex (try_lock bails silently) and skips the s_hideLocked
            // clear that the mid-hook performs alongside its writes. The next mid-hook frame then sees stale locks and
            // refuses to re-transition. The Parts= setter populates the inactive lookup buffer but does not flip
            // s_activeMap. rebuild_part_lookup() rebuilds from the stored per-category strings, honors per-character
            // overrides, and atomically publishes the new buffer, so the mid-hook and direct-write paths see edited
            // part lists on the next tick.
            const auto status = DMK::config::enable_auto_reload(
                std::chrono::milliseconds{250},
                [](bool content_changed)
                {
                    auto &logger = DMK::log();
                    if (!content_changed)
                    {
                        logger.info("INI auto-reload: skipped (no content delta)");
                        return;
                    }

                    rebuild_part_lookup();
                    update_hidden_mask();

                    auto &ps = player_state();
                    for (int i = 0; i < k_maxProtagonists; ++i)
                        ps.armorInjected[i].store(false, std::memory_order_relaxed);

                    needs_direct_write().store(true, std::memory_order_release);

                    logger.info("INI auto-reload: setters applied, visibility scheduled");
                }
            );
            if (status != DMK::config::AutoReloadStatus::Started &&
                status != DMK::config::AutoReloadStatus::AlreadyRunning)
            {
                DMK::log().warning("INI auto-reload could not start (status enum {})", static_cast<int>(status));
            }
        }
    }

    // Store the PartInOut visibility byte through the strict per-frame write. It never changes page protection, so a
    // stale or read-only target fails closed with no byte changed instead of faulting into the wrapper below.
    static void write_vis_byte(uintptr_t partInOut, uint8_t value) noexcept
    {
        (void)DMK::memory::write_in_place<std::uint8_t>(
            DMK::Address{partInOut}.offset(static_cast<std::ptrdiff_t>(vis_byte_offset())),
            value
        );
    }

    static void on_vis_check_impl(DMK::hook::MidContext &ctx)
    {
        // The relaxed load is a cheap filter. The exchange that claims the work is the synchronizing edge: it must
        // acquire against the arming store, or this thread can claim the flag and still read the previous
        // part-lookup buffer and hidden-state masks. Every site that arms the flag therefore stores it with
        // release, including the worker tails that publish a rebuilt part map and a rebuilt visCtrl set.
        if (needs_direct_write().load(std::memory_order_relaxed) &&
            needs_direct_write().exchange(false, std::memory_order_acquire))
        {
            // Clear stale locks on vis ctrl change (save load, zone transition) so chest gets a fresh first-frame
            // transition.
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
                if ((++s_debTick & 0x3F) == 0 && (steady_ms() - s_equipPending) > 500)
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

        // Part-hash key pointer, the decision function's second argument. The register that carries it moves
        // between builds, so verify it against the disassembly instead of assuming. On the current build it is RDX,
        // and the exclusion walk dereferences it as `mov ecx,[rdx]` before comparing against each entry's first
        // DWORD. That dereference is the check - a register the walk does not read THROUGH is the wrong one.
        //
        // Reading the wrong register here fails SILENTLY. The neighboring registers hold small integers (R11 carries
        // the visibility byte the hooked movzx loaded, R10 the exclusion count), so `memory::is_plausible_ptr`
        // rejects the value, this handler returns before reaching any of the logic below, and the cascade fix goes
        // dead with the hook still reporting installed.
        // See the register map on equip_vis_check() in aob_resolver.hpp.
        const auto hashPtr = DMK::hook::gpr(ctx, DMK::hook::Gpr::Rdx);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{hashPtr}))
            return;

        // Guarded read of the engine's key word. A plausible but unmapped pointer fails closed here instead of
        // faulting into the wrapper below.
        const auto hashRead = DMK::memory::read<std::uint32_t>(DMK::Address{hashPtr});
        if (!hashRead)
            return;
        const auto partHash = *hashRead;

        if (!needs_classification(partHash))
            return;

        const auto mask = classify_part(partHash);
        if (mask == 0)
            return;

        // PartInOut struct pointer, the decision function's third argument. The hooked `movzx r11d, byte [r8+0x20]`
        // reads the visibility byte through it and the branch after the exclusion walk reads its transition byte at
        // `cmp byte [r8+3],0`. Take it from R8, not from RCX: RCX carries the a1 context here, which is also a live
        // heap pointer, so it passes memory::is_plausible_ptr() and a read of the wrong register does not fail
        // loudly. The mid-hook then writes the visibility byte inside the context struct instead.
        // See the register map on equip_vis_check() in aob_resolver.hpp.
        const auto partInOut = DMK::hook::gpr(ctx, DMK::hook::Gpr::R8);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{partInOut}))
            return;

        const bool cascadeOn = flag_cascade_fix().load(std::memory_order_relaxed);
        const auto hashIdx = static_cast<uint16_t>(partHash);
        const bool isChest = (mask & category_bit(Category::Chest)) != 0;

        // Protect legs from cascade during the brief window after a hotkey toggle. Without this guard, a shield
        // show/hide triggers a re-evaluation that flashes the pants.
        if (cascadeOn)
        {
            auto guard = s_flushGuard.load(std::memory_order_relaxed);
            if (guard > 0 && s_flushGuard.compare_exchange_strong(guard, guard - 1, std::memory_order_relaxed))
            {
                if ((mask & k_cascadeBodyMask) != 0 && !is_any_category_hidden(mask) &&
                    is_category_hidden(Category::Chest))
                {
                    DMK::hook::gpr(ctx, DMK::hook::Gpr::R9) = k_selectorSkipGate;
                    return;
                }
            }
        }

        // Chest lock state machine, evaluated before the player filter. 0 = unlocked, the first frame lets the gate
        // pass. 1 = locked, the handler forces the skip-gate selector and the engine publishes no decision.
        // 2 = re-equip, the handler forces vis=0 so the In pass recreates the scene nodes.
        if (cascadeOn && isChest && s_hideLocked[hashIdx] && is_any_category_hidden(mask))
        {
            if (s_hideLocked[hashIdx] == 2)
            {
                write_vis_byte(partInOut, 0);
                s_hideLocked[hashIdx] = 0;
                return;
            }
            write_vis_byte(partInOut, 2);
            DMK::hook::gpr(ctx, DMK::hook::Gpr::R9) = k_selectorSkipGate;
            return;
        }

        // a1 visibility-control context, the decision function's first argument. check_player_filter() gates the
        // pointer itself, so this site needs no separate plausibility check.
        const auto a1 = DMK::hook::gpr(ctx, DMK::hook::Gpr::Rcx);
        if (!check_player_filter(a1))
            return;

        // Per-character override resolution. The cascade and chest-lock paths above gate engine-wide state and
        // intentionally use the GLOBAL classify_part / is_any_category_hidden masks. This block keeps the actual vis=2
        // write per-character, so an INI override that excludes a hash for one protagonist does not get hidden by the
        // mid-hook on that protagonist's frames.
        //
        // The vis-ctrl scan is O(n) where n is at most k_maxProtagonists, so the added cost is a handful of relaxed
        // atomic loads + one extra flat-table lookup per call. Relaxed ordering is sufficient: the resolve poll thread
        // publishes consistent (visCtrls[i], visCharIdx[i]) pairs on each pass, and a torn read produces at worst one
        // stale per-char idx for a single frame, well within the existing swap-detect timing tolerance.
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

        // Refine the hide decision against the per-character part map. charIdx == -1 (untracked actor) preserves the
        // legacy behavior and falls back to the global mask computed earlier.
        CategoryMask charMask = mask;
        if (charIdx >= 0 && charIdx < static_cast<int>(k_charIdxCount))
        {
            charMask = classify_part_for(partHash, charIdx);
            if (charMask == 0)
                return; // hash excluded from this character's effective Parts
        }

        if (is_any_category_hidden_for(charMask, charIdx))
        {
            write_vis_byte(partInOut, 2);
            if (cascadeOn && isChest)
            {
                // A locked frame must not publish a second transition, so close the gate instead of re-arming it.
                if (s_hideLocked[hashIdx])
                    DMK::hook::gpr(ctx, DMK::hook::Gpr::R9) = k_selectorSkipGate;
                else
                    s_hideLocked[hashIdx] = 1;
            }
        }
        else
        {
            if (cascadeOn && isChest)
                s_hideLocked[hashIdx] = 0;
        }
    }

    // SEH wrapper, a separate function because MSVC SEH cannot coexist with C++ destructors in the same frame. It is
    // the last-resort guard for a register layout that moved under an outdated mod: the game must not crash. Every
    // foreign access in the body is already a guarded memory:: call, so a fault here means a wrong register, which is
    // exactly the silent-death case the register map above warns about. Log it once.
    static void on_vis_check(DMK::hook::MidContext &ctx)
    {
        __try
        {
            on_vis_check_impl(ctx);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "EquipVisCheck: SEH caught crash in the mid-hook. Verify the register map against this build."
                );
        }
    }

    DMK::Result<void> init(DMK::Session &session, const WheelHostTable *wheel_host)
    {
        auto &logger = DMK::log();

        // Apply config before the resolver and hook-install steps so the INI LogLevel takes effect for any TRACE/DEBUG
        // emissions that follow. Setters dispatched by the INI load touch only atomics, per-category state, and input
        // bindings. None of them depend on resolved addresses, which the code below populates.
        load_config(session);

        if (!DMK::memory::init_cache())
            logger.warning("Memory cache init failed - pointer reads may be slower");

        auto &addrs = resolved_addrs();

        // One parallel pass resolves the whole registry. The targets are independent (no resolution reads
        // another's result) and all live in the host EXE, so the wall-clock collapses to the slowest single scan
        // instead of the sum. Each entry's validator rejects a value outside the image, and a code entry also rejects
        // a site that cannot begin an instruction.
        resolve_all_anchors();

        addrs.worldSystem = anchor_address(AnchorId::WorldSystem);
        addrs.childActorVtbl = anchor_address(AnchorId::ChildActorVtbl);
        addrs.mapLookup = anchor_address(AnchorId::MapLookup);
        addrs.mapInsert = anchor_address(AnchorId::MapInsert);

        // Resolve the IndexedStringA global from MapLookup: `mov rax, [rip+disp]` inside the prologue. The resolver
        // skips a decoy occurrence whose displacement lands on an implausible or unreadable address, so a compiler
        // shuffle inside the prologue still resolves.
        if (addrs.mapLookup)
        {
            const auto resolved = DMK::scan::find_and_resolve_rip_relative(
                DMK::Region{DMK::Address{addrs.mapLookup}, k_mapLookupPrologueBytes},
                DMK::scan::PREFIX_MOV_RAX_RIP,
                7
            );
            if (resolved)
            {
                addrs.indexedStringGlobal = resolved->raw();
                logger.info("IndexedStringA global resolved at 0x{:X}", addrs.indexedStringGlobal);
            }
            else
            {
                logger.warning(
                    "IndexedStringA global: no MOV RAX,[rip+disp] resolved inside the MapLookup prologue ({}), "
                    "armor injection disabled",
                    resolved.error().message()
                );
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

        original_vis_map().reserve(get_part_map().size());

        // Initial IndexedStringA scan plus the deferred-launch decision. Commit the sync attempt so the mod is
        // immediately functional, then launch the deferred worker unless the table resolves fully on the first try.
        // The worker converges on scan stability, not on a coverage percentage.
        if (addrs.mapLookup)
        {
            auto runtimeHashes = scan_indexed_string_table(addrs.mapLookup);
            const auto initialResolved = runtimeHashes.size();
            const auto totalExpected = total_part_count();

            if (initialResolved > 0)
                set_runtime_hashes(std::move(runtimeHashes));

            const bool fullyResolved = totalExpected > 0 && initialResolved == totalExpected;
            if (!fullyResolved)
            {
                logger.info(
                    "IndexedStringA scan: {}/{} entries at init, starting deferred scan thread (stability-check mode)",
                    initialResolved,
                    totalExpected
                );
                deferred_scan_pending().store(true, std::memory_order_relaxed);
                launch_deferred_scan();
            }
            else
            {
                logger.info(
                    "IndexedStringA scan: {}/{} entries at init (fully resolved, no deferred retry)",
                    initialResolved,
                    totalExpected
                );
            }

            // Rebuild the part lookup against whatever subset got committed so the active map reflects the
            // partial-or-full hash set. The deferred worker will rebuild again when it converges.
            if (initialResolved > 0)
                rebuild_part_lookup();
        }

        // Mid-body scan through the shared code-target policy. The host-EXE scope bounds this safety-critical match -
        // its callback writes engine structs (visPtr, the In/Out selector) - to CrimsonDesert.exe, where the real
        // target lives, so a generic-shaped candidate cannot first-match elsewhere in the process image. The
        // prologue-recovery fallback survives dev hot-reload: when a prior Logic-DLL generation left a detour jump at
        // this site, every original-bytes candidate fails on rescan and the resolver retries each Direct candidate
        // with its prologue rebuilt as a near JMP. The retry keeps the candidate's walk-back, so the returned address
        // still lands on the original instruction.
        const auto hookAddr = anchor_address(AnchorId::EquipVisCheck);
        if (!hookAddr)
        {
            logger.error("No AOB pattern matched. The mod may be outdated for this game version.");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "EquipHide::init"});
        }

        // Warm the self-healing vis-byte offset BEFORE the mid-hook install. The decode re-resolves the
        // EquipVisCheck instruction by AOB, and it must run before enable() patches that site.
        (void)vis_byte_offset();

        auto visCheck = DMK::hook::mid_at(
            DMK::hook::MidRequest{
                .name = "EquipVisCheck",
                .target = DMK::Address{hookAddr},
            },
            &on_vis_check
        );
        if (!visCheck)
        {
            logger.error("Hook creation failed at 0x{:X}: {}", hookAddr, visCheck.error().message());
            return std::unexpected(visCheck.error());
        }
        if (auto armed = visCheck->enable(); !armed)
        {
            logger.error("Hook could not be armed at 0x{:X}: {}", hookAddr, armed.error().message());
            return std::unexpected(armed.error());
        }
        s_hooks.push(std::move(*visCheck));
        logger.info("Hook installed at 0x{:X}", hookAddr);

        // Prevents hidden parts from flashing during state transitions (gliding exit).
        if (flag_gliding_fix().load(std::memory_order_relaxed))
        {
            const auto partAddShowAddr = anchor_address(AnchorId::PartAddShow);

            if (partAddShowAddr)
            {
                auto hook = DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = "PartAddShow",
                        .target = DMK::Address{partAddShowAddr},
                    },
                    &on_part_add_show
                );
                if (!hook)
                {
                    logger.warning("PartAddShow hook failed: {} - gliding flash fix disabled", hook.error().message());
                }
                else
                {
                    // Publish the trampoline BEFORE enable() arms the patch.
                    set_part_add_show_trampoline(hook->original<PartAddShowFn>());
                    if (auto armed = hook->enable(); !armed)
                        logger.warning(
                            "PartAddShow hook could not be armed: {} - gliding flash fix disabled",
                            armed.error().message()
                        );
                    else
                        s_hooks.push(std::move(*hook));
                }
            }
            else
            {
                logger.warning("PartAddShow AOB scan failed - gliding flash fix disabled");
            }
        }

        // Keeps the hair when a helmet or cloak is hidden. The hook temporarily sets bit 19 in item+0x70 bitmasks
        // per-call, so PostfixEval sees inactive priority and no hair rule matches. A call-graph landmark separates
        // player invocations from NPC invocations: NPC PostfixEval calls always traverse one specific caller, and an
        // AOB resolves that caller's return address (npcPfeReturnAddr). The hook scans its own stack window for that
        // landmark and rejects NPC calls.
        if (flag_bald_fix().load(std::memory_order_relaxed))
        {
            const auto postfixEvalAddr = anchor_address(AnchorId::PostfixEval);

            // The row's own walk-back already reaches the byte after the rule-eval call, so the resolved value IS the
            // return address an NPC stack frame carries. Do NOT add a fixup here. The offset belongs in the
            // candidate row, and a second application yields an address no call ever pushes, which makes
            // is_npc_call_stack() reject every call while the install still logs "bald fix active".
            addrs.npcPfeReturnAddr = anchor_address(AnchorId::NpcPfeReturnAddr);

            if (postfixEvalAddr && addrs.npcPfeReturnAddr)
            {
                auto hook = DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = "PostfixEval",
                        .target = DMK::Address{postfixEvalAddr},
                    },
                    &on_postfix_eval
                );
                if (!hook)
                {
                    logger.warning("PostfixEval hook failed: {} - bald fix disabled", hook.error().message());
                }
                else
                {
                    set_postfix_eval_trampoline(hook->original<PostfixEvalFn>());
                    if (auto armed = hook->enable(); !armed)
                    {
                        logger.warning(
                            "PostfixEval hook could not be armed: {} - bald fix disabled",
                            armed.error().message()
                        );
                    }
                    else
                    {
                        s_hooks.push(std::move(*hook));
                        logger.info(
                            "PostfixEval inline hook installed at 0x{:X}, npc-caller landmark 0x{:X} - bald fix active",
                            postfixEvalAddr,
                            addrs.npcPfeReturnAddr
                        );
                    }
                }
            }
            else if (!postfixEvalAddr)
            {
                logger.warning("PostfixEval AOB scan failed - bald fix disabled");
            }
            else
            {
                logger.warning(
                    "NpcPfeReturnAddr AOB scan failed - bald fix disabled "
                    "(refusing to run without the call-graph filter)"
                );
            }
        }
        else
        {
            logger.info("BaldFix disabled in config - the engine hair rules apply normally");
        }

        // Equipment change detection for CascadeFix re-sync. Clears the gate-skip locks when chest armor changes, so
        // the new gear gets a fresh Out transition.
        if (flag_cascade_fix().load(std::memory_order_relaxed))
        {
            const auto vecAddr = anchor_address(AnchorId::VisualEquipChange);
            if (vecAddr)
            {
                auto hook = DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = "VisualEquipChange",
                        .target = DMK::Address{vecAddr},
                    },
                    &on_visual_equip_change
                );
                if (!hook)
                {
                    logger.warning("VisualEquipChange hook failed: {}", hook.error().message());
                }
                else
                {
                    set_visual_equip_change_trampoline(hook->original<VisualEquipChangeFn>());
                    if (auto armed = hook->enable(); !armed)
                        logger.warning("VisualEquipChange hook could not be armed: {}", armed.error().message());
                    else
                        s_hooks.push(std::move(*hook));
                }
            }

            const auto vesAddr = anchor_address(AnchorId::VisualEquipSwap);
            if (vesAddr)
            {
                auto hook = DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = "VisualEquipSwap",
                        .target = DMK::Address{vesAddr},
                    },
                    &on_visual_equip_swap
                );
                if (!hook)
                {
                    logger.warning("VisualEquipSwap hook failed: {}", hook.error().message());
                }
                else
                {
                    set_visual_equip_swap_trampoline(hook->original<VisualEquipSwapFn>());
                    if (auto armed = hook->enable(); !armed)
                        logger.warning("VisualEquipSwap hook could not be armed: {}", armed.error().message());
                    else
                        s_hooks.push(std::move(*hook));
                }
            }
        }

        // load_config() bound the hotkeys, so the INI load already picked up their combo keys. Now bring the poll
        // engine live. The wheel backend is the loader's resident host when the dev loader supplied one, so a
        // user-bound mouse-wheel combo books its permanent keepalive against that module instead of this one and the
        // logic DLL stays unmappable. Without a host (the production ASI, and a dev loader whose host failed to
        // start) the local message hook is the correct backend and takes that keepalive here.
        DMK::input::Input::Settings inputSettings{};
        if (wheel_host != nullptr)
        {
            inputSettings.wheel_backend = DMK::input::Input::WheelBackend::ExternalHost;
            inputSettings.wheel_host = wheel_host;
            inputSettings.wheel_host_required = false;
        }
        if (auto started = session.input().start(inputSettings); !started)
        {
            logger.warning("Input engine did not start: {} - hotkeys are inactive", started.error().message());
        }

        // Drives resolve_player_vis_ctrls on a fixed cadence so the mod detects a cold load and an in-session
        // character swap without the EquipVisCheck hook's event stream. See background_threads.cpp for the thread
        // body.
        launch_resolve_poll();

        if (deferred_scan_pending().load(std::memory_order_relaxed))
            logger.info("Hooks installed, part hashes pending (deferred scan active)");
        else
            logger.info("Equip hide fully initialized ({} parts resolved)", total_part_count());

        // One-shot DMK health snapshot for at-a-glance per-launch diagnostics: hook population plus any intentional
        // loader-lock leak/detach events. The build token stamps the game image the anchors resolved against, so a
        // log from an unknown patch identifies itself. It is LAYOUT identity: an in-place code patch that leaves the
        // PE timestamp, SizeOfImage and section table intact produces the same token.
        const auto health = DMK::diagnostics::collect({}, anchor_report());
        const auto image = DMK::scan::image_identity();
        logger.info(
            "DMK health: hooks total={} active={} disabled={}, intentional-leaks={}, build-token=0x{:016X}",
            health.hooks_total,
            health.hooks_active,
            health.hooks_disabled,
            health.total_intentional_leaks,
            image.token()
        );

        return {};
    }

    void arm_flush_guard() noexcept
    {
        if (flag_cascade_fix().load(std::memory_order_relaxed))
            s_flushGuard.store(500, std::memory_order_relaxed);
    }

    bool shutdown()
    {
        auto &logger = DMK::log();
        logger.info("{} shutting down...", MOD_NAME);

        // Module pins, by reason, sampled HERE rather than at the end of init(). The input engine mounts its wheel
        // route and installs its XInput interception from the poll thread's cycle loop, which starts only after
        // session.input().start() returns. An init-time sample therefore reads a state where neither step ran yet
        // and reports zeros that mean nothing. This runs before any teardown, so the hooks and pins are both still
        // in place.
        //
        // The engine takes XInputKeepalive before XInput hook creation. A nonzero count means a binding asked to
        // consume and the interception pair went in. That set stays and goes inert after teardown, so it keeps this
        // image mapped and costs one generation against the dev loader's reload budget.
        //
        // MessageHookKeepalive is the wheel-capture keepalive. In the dev build this reads zero: the resident loader
        // owns wheel capture, so the pin lands on the loader and this generation can still unmap. A nonzero count
        // means the engine refused the external host and fell back to a local message hook.
        {
            const auto pins = DMK::diagnostics::collect();
            const auto pin = [&pins](DMK::diagnostics::ModulePinReason reason)
            { return pins.module_pins[static_cast<std::size_t>(reason)]; };

            DMK::log().info(
                "DMK pins: xinput_self={} xinput_targets={} wheel_msghook={} input_poller={} total={}",
                pin(DMK::diagnostics::ModulePinReason::XInputKeepalive),
                pin(DMK::diagnostics::ModulePinReason::XInputTarget),
                pin(DMK::diagnostics::ModulePinReason::MessageHookKeepalive),
                pin(DMK::diagnostics::ModulePinReason::InputPoller),
                pins.total_module_pins
            );
        }

        // Per-step bracket logs around each blocking call pin a shutdown stall to the exact step (worker join,
        // vis-byte cleanup, hook teardown, and so on). The teardown path crosses several mutexes and the hook drain
        // window. Without the brackets, a silent hang stays undiagnosable from the user's log alone. flush() between
        // steps drains the async queue, so a hang inside a step still surfaces every line the prior step emitted.
        logger.info("{} shutdown: step 1 disable_auto_reload", MOD_NAME);
        // Disable the INI watcher up front so an in-flight save event cannot fire setters during the state teardown.
        DMK::config::disable_auto_reload();
        logger.flush();

        logger.info("{} shutdown: step 2 signal stop", MOD_NAME);
        shutdown_requested().store(true, std::memory_order_relaxed);
        lazy_probe_pending().store(false, std::memory_order_relaxed);
        logger.flush();

        logger.info("{} shutdown: step 3 join workers", MOD_NAME);
        // Drain workers before the hooks they call into come down. shutdown_requested is the cooperative stop signal
        // that each StoppableWorker body polls. A join here guarantees that no worker is mid-call into a trampoline
        // when the loader unmaps the trampoline pages.
        join_background_threads();
        logger.flush();

        logger.info("{} shutdown: step 4 cleanup vis bytes", MOD_NAME);
        // Restore visibility bytes while the hooks are still installed and the game's part registry is reachable. A
        // run after the hook teardown races the loader, which unmaps the Logic-DLL pages that back the cleanup
        // function itself.
        cleanup_vis_bytes();
        logger.flush();

        logger.info("{} shutdown: step 5 restore hooked prologues", MOD_NAME);
        // Newest-first teardown of every hook this mod owns. A teardown that cannot prove the restore pins the
        // backend instead and books one leak against LeakSubsystem::HookManager, so the delta across the clear IS
        // the unmap verdict the dev loader needs. Compare the delta and never the absolute: the same counter also
        // carries caller-requested leaks from elsewhere. Each detour body snapshots its trampoline pointer at entry
        // and falls back to a benign default when the snapshot is null, which defends the drain window between the
        // restore and the DLL unmap.
        const auto pinsBefore = DMK::diagnostics::intentional_leak_count(DMK::diagnostics::LeakSubsystem::HookManager);
        s_hooks.clear();
        const bool restored =
            DMK::diagnostics::intentional_leak_count(DMK::diagnostics::LeakSubsystem::HookManager) == pinsBefore;
        if (!restored)
        {
            s_hookRestoreFailed.store(true, std::memory_order_relaxed);
            logger.error("{} shutdown: a hooked prologue could not be restored and stays pinned", MOD_NAME);
        }
        logger.info("{} shutdown complete", MOD_NAME);
        logger.flush();
        return restored && !s_hookRestoreFailed.load(std::memory_order_relaxed);
    }

} // namespace EquipHide
