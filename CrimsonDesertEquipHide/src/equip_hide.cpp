#include "equip_hide.hpp"
#include "aob_resolver.hpp"
#include "armor_injection.hpp"
#include "background_threads.hpp"
#include "bald_fix.hpp"
#include "categories.hpp"
#include "constants.hpp"
#include "gliding_fix.hpp"
#include "indexed_string_table.hpp"
#include "input_handler.hpp"
#include "player_detection.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <string>
#include <vector>

namespace EquipHide
{
    // --- Config ---
    static void load_config()
    {
        DMK::Config::register_string("General", "LogLevel", "Log Level", [](const std::string &val)
                                     {
                auto& logger = DMK::Logger::get_instance();
                logger.set_log_level(DMK::Logger::string_to_log_level(val)); }, "Info");

        DMK::Config::register_bool("General", "PlayerOnly", "Player Only", [](bool val)
                                   { flag_player_only().store(val, std::memory_order_relaxed); }, true);

        DMK::Config::register_bool("General", "ForceShow", "Force Show", [](bool val)
                                   { flag_force_show().store(val, std::memory_order_relaxed); }, false);

        DMK::Config::register_bool("General", "BaldFix", "Bald Fix", [](bool val)
                                   { flag_bald_fix().store(val, std::memory_order_relaxed); }, true);

        DMK::Config::register_bool("General", "GlidingFix", "Gliding Fix", [](bool val)
                                   { flag_gliding_fix().store(val, std::memory_order_relaxed); }, true);

        DMK::Config::register_bool("General", "IndependentToggle", "Independent Toggle", [](bool val)
                                   { flag_independent_toggle().store(val, std::memory_order_relaxed); }, false);

        DMK::Config::register_key_combo("General", "ShowAllHotkey", "Show All Hotkey", [](const DMK::Config::KeyComboList &combos)
                                        { set_show_all_combos(combos); }, "");

        DMK::Config::register_key_combo("General", "HideAllHotkey", "Hide All Hotkey", [](const DMK::Config::KeyComboList &combos)
                                        { set_hide_all_combos(combos); }, "");

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto cat = static_cast<Category>(i);
            const std::string section{category_section(cat)};

            const bool active = (cat == Category::Shields ||
                                 cat == Category::Helm ||
                                 cat == Category::Mask);
            const char *defaultToggle = active ? "V" : "";

            DMK::Config::register_bool(section, "Enabled", section + " Enabled", [i](bool val)
                                       { category_states()[i].enabled.store(val, std::memory_order_relaxed); }, active);

            DMK::Config::register_key_combo(section, "ToggleHotkey", section + " Toggle Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].toggleHotkeyCombos = combos; }, defaultToggle);

            DMK::Config::register_key_combo(section, "ShowHotkey", section + " Show Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].showHotkeyCombos = combos; }, "");

            DMK::Config::register_key_combo(section, "HideHotkey", section + " Hide Hotkey", [i](const DMK::Config::KeyComboList &combos)
                                            { category_states()[i].hideHotkeyCombos = combos; }, "");

            DMK::Config::register_bool(section, "DefaultHidden", section + " Default Hidden", [i](bool val)
                                       { category_states()[i].hidden.store(val, std::memory_order_relaxed); }, active);

            DMK::Config::register_string(section, "Parts", section + " Parts", [cat](const std::string &val)
                                         { register_parts(cat, val); }, default_parts_string(cat));
        }

        DMK::Config::load(INI_FILE);
        DMK::Config::log_all();
        build_part_lookup();
        update_hidden_mask();

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
            inject_armor_entries();
            apply_direct_vis_write();
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

        auto r13 = ctx.r13;
        if (r13 < 0x10000)
            return;

        auto a1 = *reinterpret_cast<uintptr_t *>(ctx.rbp + 0x4F);
        if (!check_player_filter(a1))
            return;

        if (is_any_category_hidden(mask))
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(r13 + 0x1C);
            *visPtr = 2;
        }
        else if (flag_force_show().load(std::memory_order_relaxed))
        {
            auto *visPtr = reinterpret_cast<uint8_t *>(r13 + 0x1C);
            *visPtr = 0;
        }
    }

    /* SEH wrapper: separate function because MSVC SEH cannot coexist with
       C++ destructors in the same frame. Swallows faults if the mod is
       outdated and register layout has changed — don't crash the game. */
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

        logger.info("{} v{}", MOD_NAME, MOD_VERSION);
        logger.info("By {}", MOD_AUTHOR);
        logger.info("Source: {}", MOD_SOURCE);
        logger.info("Nexus:  {}", MOD_NEXUS);
        logger.debug("Built on " __DATE__ " at " __TIME__);

        DMK::Memory::init_cache();

        auto &addrs = resolved_addrs();

        addrs.worldSystem = resolve_address(
            k_worldSystemCandidates, std::size(k_worldSystemCandidates),
            "WorldSystem");

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

        if (addrs.mapLookup)
        {
            auto runtimeHashes = scan_indexed_string_table(addrs.mapLookup);
            if (!runtimeHashes.empty())
            {
                logger.info("IndexedStringA scan: {} entries resolved at init",
                            runtimeHashes.size());
                set_runtime_hashes(std::move(runtimeHashes));
            }
            else
            {
                logger.info("IndexedStringA table not ready at init, "
                            "starting deferred scan thread");
                deferred_scan_pending().store(true, std::memory_order_relaxed);
                launch_deferred_scan();
            }
        }
        else
        {
            logger.warning("MapLookup not resolved, cannot scan IndexedStringA table");
        }

        load_config();
        original_vis_map().reserve(get_part_map().size());

        std::vector<CompiledCandidate> compiledCandidates;
        for (const auto &candidate : k_hookSiteCandidates)
        {
            auto compiled = DMK::Scanner::parse_aob(candidate.pattern);
            if (compiled)
                compiledCandidates.push_back({&candidate, std::move(*compiled)});
            else
                logger.warning("Failed to parse AOB pattern '{}'", candidate.name);
        }

        if (compiledCandidates.empty())
        {
            logger.error("No valid AOB patterns available.");
            return false;
        }

        const AobCandidate *matchedSource = nullptr;
        uintptr_t hookAddr = scan_for_hook_target(compiledCandidates, matchedSource);

        if (hookAddr == 0)
        {
            logger.error("No AOB pattern matched. The mod may be outdated for this game version.");
            return false;
        }

        auto &hookMgr = DMK::HookManager::get_instance();
        auto hookResult = hookMgr.create_mid_hook("EquipVisCheck", hookAddr, on_vis_check);

        if (!hookResult.has_value())
        {
            logger.error("Hook creation failed at 0x{:X}: {}",
                         hookAddr, DetourModKit::Hook::error_to_string(hookResult.error()));
            return false;
        }

        logger.info("Hook installed via pattern '{}' at 0x{:X}",
                    matchedSource->name, hookAddr);

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
                    logger.warning("PartAddShow hook failed: {} — gliding flash fix disabled",
                                   DetourModKit::Hook::error_to_string(result.error()));
            }
            else
            {
                logger.warning("PartAddShow AOB scan failed — gliding flash fix disabled");
            }
        }

        // Prevents baldness when hiding helmets/cloaks by suppressing
        // the game's postfix rules that hide hair based on equipped gear.
        if (flag_bald_fix().load(std::memory_order_relaxed))
        {
            auto postfixEvalAddr = resolve_address(
                k_postfixEvalCandidates, std::size(k_postfixEvalCandidates),
                "PostfixEval");

            if (postfixEvalAddr)
            {
                PostfixEvalFn trampoline = nullptr;
                auto result = hookMgr.create_inline_hook(
                    "PostfixEval", postfixEvalAddr,
                    reinterpret_cast<void *>(on_postfix_eval),
                    reinterpret_cast<void **>(&trampoline));

                if (result.has_value())
                {
                    set_postfix_eval_trampoline(trampoline);
                    logger.info("PostfixEval inline hook installed at 0x{:X} — bald fix active",
                                postfixEvalAddr);
                }
                else
                    logger.warning("PostfixEval hook failed: {} — bald fix disabled",
                                   DetourModKit::Hook::error_to_string(result.error()));
            }
            else
            {
                logger.warning("PostfixEval AOB scan failed — bald fix disabled");
            }
        }
        else
        {
            logger.info("BaldFix disabled in config — hair-hiding rules will apply normally");
        }

        register_hotkeys();

        auto &inputMgr = DMK::InputManager::get_instance();
        inputMgr.start();

        if (deferred_scan_pending().load(std::memory_order_relaxed))
            logger.info("Hooks installed, part hashes pending (deferred scan active)");
        else
            logger.info("Equip hide fully initialized ({} parts resolved)",
                        total_part_count());
        return true;
    }

    void shutdown()
    {
        DMK::Logger::get_instance().info("{} shutting down...", MOD_NAME);
        shutdown_requested().store(true, std::memory_order_relaxed);
        lazy_probe_pending().store(false, std::memory_order_relaxed);

        join_background_threads();

        cleanup_vis_bytes();
        DMK_Shutdown();
    }

} // namespace EquipHide
