#include "transmog.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "indexed_string_table.hpp"
#include "input_handler.hpp"
#include "item_name_table.hpp"
#include "part_show_suppress.hpp"
#include "preset_manager.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "transmog_apply.hpp"
#include "transmog_hooks.hpp"
#include "transmog_map.hpp"
#include "transmog_worker.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <string>

namespace Transmog
{
    // --- Config ---

    static void load_config()
    {
        DMK::Config::register_string(
            "General", "LogLevel", "Log Level",
            [](const std::string &val)
            {
                auto &logger = DMK::Logger::get_instance();
                logger.set_log_level(DMK::Logger::string_to_log_level(val));
            },
            "Info");

        DMK::Config::register_bool(
            "General", "Enabled", "Enabled",
            [](bool val)
            {
                flag_enabled().store(val, std::memory_order_relaxed);
            },
            true);

        DMK::Config::register_bool(
            "General", "PlayerOnly", "Player Only",
            [](bool val)
            {
                flag_player_only().store(val, std::memory_order_relaxed);
            },
            true);

        DMK::Config::register_key_combo(
            "General", "ToggleHotkey", "Toggle Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_toggle_combos(combos);
            },
            "");

        DMK::Config::register_key_combo(
            "General", "ApplyHotkey", "Apply Transmog Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_apply_combos(combos);
            },
            "");

        DMK::Config::register_key_combo(
            "General", "ClearHotkey", "Clear Transmog Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_clear_combos(combos);
            },
            "");

        DMK::Config::register_key_combo(
            "General", "CaptureHotkey", "Capture Outfit Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_capture_combos(combos);
            },
            "");

        DMK::Config::register_key_combo(
            "Presets", "AppendHotkey", "Append Preset Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_preset_append_combos(combos);
            },
            "");

        DMK::Config::register_key_combo(
            "Presets", "ReplaceHotkey", "Replace Preset Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_preset_replace_combos(combos);
            },
            "");

        DMK::Config::register_key_combo(
            "Presets", "RemoveHotkey", "Remove Preset Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_preset_remove_combos(combos);
            },
            "");

        DMK::Config::register_key_combo(
            "Presets", "NextHotkey", "Next Preset Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_preset_next_combos(combos);
            },
            "");

        DMK::Config::register_key_combo(
            "Presets", "PrevHotkey", "Previous Preset Hotkey",
            [](const DMK::Config::KeyComboList &combos)
            {
                set_preset_prev_combos(combos);
            },
            "");

        DMK::Config::load(INI_FILE);
        DMK::Config::log_all();
    }

    // --- Public interface ---

    static __int64 get_player_a1()
    {
        auto a1 = player_a1().load(std::memory_order_acquire);
        if (a1 > 0x10000)
            return a1;

        a1 = resolve_player_component();
        if (a1 > 0x10000)
            player_a1().store(a1, std::memory_order_release);
        return a1;
    }

    bool is_world_ready() noexcept
    {
        if (!world_system_ptr().load(std::memory_order_acquire))
            return false;
        return resolve_player_component() > 0x10000;
    }

    void manual_apply()
    {
        if (!is_world_ready())
        {
            DMK::Logger::get_instance().debug("Manual apply: player not found");
            return;
        }

        DMK::Logger::get_instance().debug("Manual apply: scheduling (debounced)");
        clear_pending().store(false, std::memory_order_release);
        // Reset to "all slots" so the worker runs the full path.
        pending_slot_index().store(k_slotCount, std::memory_order_release);
        schedule_transmog_ms(k_manualDebounceMs);
    }

    void manual_apply_slot(std::size_t slotIdx)
    {
        if (!is_world_ready())
        {
            DMK::Logger::get_instance().debug(
                "Manual apply slot={}: player not found", slotIdx);
            return;
        }

        DMK::Logger::get_instance().debug(
            "Manual apply slot={}: scheduling (debounced)", slotIdx);
        clear_pending().store(false, std::memory_order_release);
        pending_slot_index().store(slotIdx, std::memory_order_release);
        schedule_transmog_ms(k_manualDebounceMs);
    }

    void manual_clear()
    {
        if (!is_world_ready())
        {
            DMK::Logger::get_instance().warning("Manual clear: player not found");
            return;
        }

        DMK::Logger::get_instance().info("Manual clear: scheduling (debounced)");
        clear_pending().store(true, std::memory_order_release);
        schedule_transmog_ms(k_manualDebounceMs);
    }

    void capture_outfit()
    {
        auto &logger = DMK::Logger::get_instance();
        auto a1 = get_player_a1();
        if (a1 < 0x10000)
        {
            logger.info("Capture: player not found");
            return;
        }

        __try
        {
            auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + 120);
            if (entryDesc < 0x10000) { logger.warning("Capture: no entry table"); return; }
            auto entryArray = *reinterpret_cast<uintptr_t *>(entryDesc + 8);
            auto entryCount = *reinterpret_cast<uint32_t *>(entryDesc + 16);

            logger.info("=== CAPTURE: {} equipment slots ===", entryCount);

            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                slot_mappings()[i].active = true;
                slot_mappings()[i].targetItemId = 0;
            }

            int captured = 0;
            for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
            {
                auto base = entryArray + e * 200;
                auto gameSlot = *reinterpret_cast<int16_t *>(base + 192);
                auto itemId = *reinterpret_cast<uint16_t *>(base + 8);

                logger.info("  Slot {:>2} ({:<12}) = item {:#06x}",
                            gameSlot, game_slot_name(gameSlot), itemId);

                auto tmSlot = slot_from_game_slot(gameSlot);
                if (tmSlot.has_value() && itemId != 0 && itemId != 0xFFFF)
                {
                    auto idx = static_cast<std::size_t>(*tmSlot);
                    slot_mappings()[idx].targetItemId = itemId;
                    ++captured;
                    logger.info("    -> Captured as {} target", slot_name(*tmSlot));
                }
            }

            logger.info("=== CAPTURE DONE: {} equipped, {} total slots ===",
                        captured, k_slotCount);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("Capture: access fault");
        }
    }

    void capture_real_equipment()
    {
        auto &logger = DMK::Logger::get_instance();
        auto a1 = get_player_a1();
        if (a1 < 0x10000)
        {
            logger.info("capture_real_equipment: player not found");
            return;
        }

        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            slot_mappings()[i].active = true;
            slot_mappings()[i].targetItemId = 0;
        }

        __try
        {
            auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + 120);
            if (entryDesc < 0x10000) return;
            auto entryArray = *reinterpret_cast<uintptr_t *>(entryDesc + 8);
            auto entryCount = *reinterpret_cast<uint32_t *>(entryDesc + 16);

            for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
            {
                auto base = entryArray + e * 200;
                auto gameSlot = *reinterpret_cast<int16_t *>(base + 192);
                auto itemId = *reinterpret_cast<uint16_t *>(base + 8);

                auto tmSlot = slot_from_game_slot(gameSlot);
                if (tmSlot.has_value() && itemId != 0 && itemId != 0xFFFF)
                    slot_mappings()[static_cast<std::size_t>(*tmSlot)].targetItemId = itemId;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("capture_real_equipment: access fault");
        }
    }

    // --- Init / Shutdown ---

    bool init()
    {
        auto &logger = DMK::Logger::get_instance();

        logger.info("{} v{}", MOD_NAME, MOD_VERSION);
        logger.info("By {}", MOD_AUTHOR);
        logger.info("Source: {}", MOD_SOURCE);
        logger.debug("Built on " __DATE__ " at " __TIME__);

        if (!DMK::Memory::init_cache())
            logger.warning("Memory cache init failed — pointer reads may be slower");

        // --- Resolve AOB addresses ---

        auto &addrs = resolved_addrs();

        // SlotPopulator: the KEY function for transmog.
        addrs.slotPopulator = resolve_address(
            k_slotPopulatorCandidates,
            std::size(k_slotPopulatorCandidates),
            "SlotPopulator");

        if (!addrs.slotPopulator)
            logger.warning("SlotPopulator AOB scan failed — transmog will not work");

        // MapLookup: IndexedStringA::lookup. Not hooked — RIP anchor for
        // scan_indexed_string_table(). Must be resolved before
        // PartShowSuppress::init_slot_hashes.
        addrs.mapLookup = resolve_address(
            k_mapLookupCandidates,
            std::size(k_mapLookupCandidates),
            "MapLookup");

        if (addrs.mapLookup)
        {
            auto nameToHash = scan_indexed_string_table(addrs.mapLookup);
            if (!nameToHash.empty())
            {
                auto resolved = PartShowSuppress::init_slot_hashes(nameToHash);
                logger.info(
                    "[dispatch] slot hashes initialized: {}/{} slots resolved",
                    resolved, k_slotCount);
            }
            else
            {
                logger.debug(
                    "[dispatch] IndexedStringA scan returned empty — "
                    "slot hashes unresolved, PartShowSuppress disabled");
            }
        }
        else
        {
            logger.warning(
                "MapLookup AOB scan failed — cannot resolve CD_* slot hashes, "
                "PartShowSuppress will be inert this session");
        }

        // SubTranslator (sub_14076D950): anchor for the item-name catalog scan.
        addrs.subTranslator = resolve_address(
            k_subTranslatorCandidates,
            std::size(k_subTranslatorCandidates),
            "SubTranslator");

        if (addrs.subTranslator)
        {
            using BR = ItemNameTable::BuildResult;
            const auto result = ItemNameTable::instance().build(addrs.subTranslator);
            if (result == BR::Ok)
            {
                logger.info(
                    "[nametable] built synchronously at init "
                    "({} entries)",
                    ItemNameTable::instance().size());
                if (logger.is_enabled(DMK::LogLevel::Trace))
                    ItemNameTable::instance().dump_catalog_tsv();
            }
            else if (result == BR::Deferred)
            {
                logger.info(
                    "[nametable] iteminfo global not initialized yet — "
                    "starting background scan thread");
                launch_deferred_nametable_scan();
            }
            else // Fatal
            {
                logger.warning(
                    "[nametable] address chain resolution failed — "
                    "item-name table disabled this session");
            }

            addrs.indexedStringLookup =
                ItemNameTable::instance().indexed_string_lookup_addr();
            if (addrs.indexedStringLookup)
            {
                logger.info("IndexedStringLookup cached at 0x{:X} (via chain walk)",
                            addrs.indexedStringLookup);
            }
        }
        else
        {
            logger.warning(
                "SubTranslator AOB scan failed — cannot build item-name table, "
                "presets will fall back to raw itemId only");
        }

        // SafeTearDown (sub_14075FE60): scene-graph tear-down.
        addrs.safeTearDown = resolve_address(
            k_safeTearDownCandidates,
            std::size(k_safeTearDownCandidates),
            "SafeTearDown");
        if (!addrs.safeTearDown)
        {
            logger.warning(
                "SafeTearDown AOB scan failed — real_part_tear_down will "
                "be disabled this session");
        }

        // InitSwapEntry (sub_141D451B0): zero-init helper for the 0x80-byte
        // swap entry passed to SlotPopulator.
        {
            auto iseAddr = resolve_address(
                k_initSwapEntryCandidates,
                std::size(k_initSwapEntryCandidates),
                "InitSwapEntry");

            if (iseAddr && !sanity_check_function_prologue(iseAddr))
            {
                logger.warning(
                    "InitSwapEntry resolved to 0x{:X} but prologue byte "
                    "looks wrong — rejecting",
                    iseAddr);
                iseAddr = 0;
            }

            if (iseAddr)
            {
                init_swap_entry_fn() = reinterpret_cast<InitSwapEntryFn>(iseAddr);
                logger.info("InitSwapEntry at 0x{:X}", iseAddr);
            }
            else
            {
                logger.warning(
                    "InitSwapEntry AOB scan failed — transmog apply will "
                    "be disabled this session");
            }
        }

        // CharClassBypass: single-byte patch site in CondPrefab evaluator.
        // Toggled 0x74↔0xEB around each carrier apply so NPC items
        // pass the character-class hash check.
        addrs.charClassBypass = resolve_address(
            k_charClassBypassCandidates,
            std::size(k_charClassBypassCandidates),
            "CharClassBypass");
        if (addrs.charClassBypass)
        {
            // Verify the resolved byte is 0x74 (jz). SEH-isolated via
            // noinline lambda (C++ objects in parent block unwinding).
            uint8_t probe = 0;
            [&]() __declspec(noinline) {
                __try { probe = *reinterpret_cast<volatile uint8_t *>(addrs.charClassBypass); }
                __except (EXCEPTION_EXECUTE_HANDLER) { probe = 0; }
            }();
            if (probe == 0x74)
                logger.info("CharClassBypass at 0x{:X} (byte=0x{:02X} OK)",
                            addrs.charClassBypass, probe);
            else
            {
                logger.warning("CharClassBypass at 0x{:X} byte=0x{:02X} "
                               "(expected 0x74) -- disabling",
                               addrs.charClassBypass, probe);
                addrs.charClassBypass = 0;
            }
        }
        else
        {
            logger.warning(
                "CharClassBypass AOB scan failed -- NPC-variant transmog "
                "will fall back to direct apply (may not render)");
        }

        // --- Load config ---

        load_config();

        // --- Load presets ---

        {
            std::wstring rtDirW = DMK::Filesystem::get_runtime_directory();
            std::string rtDir;
            if (!rtDirW.empty())
            {
                int needed = WideCharToMultiByte(
                    CP_UTF8, 0,
                    rtDirW.data(), static_cast<int>(rtDirW.size()),
                    nullptr, 0, nullptr, nullptr);
                if (needed > 0)
                {
                    rtDir.resize(static_cast<std::size_t>(needed));
                    WideCharToMultiByte(
                        CP_UTF8, 0,
                        rtDirW.data(), static_cast<int>(rtDirW.size()),
                        rtDir.data(), needed, nullptr, nullptr);
                }
            }
            if (!rtDir.empty() && rtDir.back() != '\\' && rtDir.back() != '/')
                rtDir.push_back('\\');
            std::string presetsPath = rtDir + PRESETS_FILE;

            auto &pm = PresetManager::instance();
            pm.load(presetsPath);
            pm.apply_to_state();

            logger.info("Active character: '{}', preset {}/{}",
                        pm.active_character(),
                        pm.preset_count() > 0 ? pm.active_preset_index() + 1 : 0,
                        pm.preset_count());
        }

        // --- Install hooks ---

        auto &hookMgr = DMK::HookManager::get_instance();

        // SlotPopulator: resolved for direct call (not hooked).
        if (addrs.slotPopulator)
        {
            if (sanity_check_function_prologue(addrs.slotPopulator))
            {
                slot_populator_fn() =
                    reinterpret_cast<SlotPopulatorFn>(addrs.slotPopulator);
                logger.info("SlotPopulator resolved at 0x{:X}", addrs.slotPopulator);
            }
            else
            {
                logger.warning(
                    "SlotPopulator resolved to 0x{:X} but prologue byte "
                    "looks wrong — rejecting, transmog apply disabled",
                    addrs.slotPopulator);
                addrs.slotPopulator = 0;
            }
        }

        // BatchEquip: THE equip trigger function.
        {
            auto beAddr = resolve_address(
                k_batchEquipCandidates,
                std::size(k_batchEquipCandidates),
                "BatchEquip");

            if (beAddr && !sanity_check_function_prologue(beAddr))
            {
                logger.warning(
                    "BatchEquip resolved to 0x{:X} but prologue byte "
                    "looks wrong — rejecting",
                    beAddr);
                beAddr = 0;
            }

            if (beAddr)
            {
                BatchEquipFn trampoline = nullptr;
                auto result = hookMgr.create_inline_hook(
                    "BatchEquip", beAddr,
                    reinterpret_cast<void *>(on_batch_equip),
                    reinterpret_cast<void **>(&trampoline));

                if (result.has_value())
                {
                    orig_batch_equip() = trampoline;
                    logger.info("BatchEquip hook installed at 0x{:X}", beAddr);
                }
                else
                {
                    logger.warning("BatchEquip hook failed: {}",
                                   DetourModKit::Hook::error_to_string(result.error()));
                }
            }
            else
            {
                logger.warning("BatchEquip AOB scan failed — transmog equip trigger disabled");
            }
        }

        // VEC hook: catches unequip events to re-apply transmog.
        {
            auto vecAddr = resolve_address(
                k_vecCandidates, std::size(k_vecCandidates), "VEC");
            if (vecAddr && !sanity_check_function_prologue(vecAddr))
            {
                logger.warning(
                    "VEC resolved to 0x{:X} but prologue byte looks "
                    "wrong — rejecting",
                    vecAddr);
                vecAddr = 0;
            }
            if (vecAddr)
            {
                VisualEquipChangeFn trampoline = nullptr;
                auto result = hookMgr.create_inline_hook(
                    "VEC", vecAddr,
                    reinterpret_cast<void *>(on_vec),
                    reinterpret_cast<void **>(&trampoline));
                if (result.has_value())
                {
                    orig_vec() = trampoline;
                    logger.info("VEC hook installed at 0x{:X}", vecAddr);
                }
                else
                {
                    logger.warning("VEC hook failed: {}",
                                   DetourModKit::Hook::error_to_string(result.error()));
                }
            }
        }

        // PartAddShow (sub_14081DC20) inline hook.
        {
            auto pasAddr = resolve_address(
                k_partAddShowCandidates,
                std::size(k_partAddShowCandidates),
                "PartAddShow");

            if (pasAddr && !sanity_check_function_prologue(pasAddr))
            {
                logger.warning(
                    "PartAddShow resolved to 0x{:X} but prologue byte "
                    "looks wrong — rejecting",
                    pasAddr);
                pasAddr = 0;
            }

            if (pasAddr)
            {
                PartShowSuppress::PartAddShowFn trampoline = nullptr;
                auto result = hookMgr.create_inline_hook(
                    "PartAddShow", pasAddr,
                    reinterpret_cast<void *>(PartShowSuppress::on_part_add_show),
                    reinterpret_cast<void **>(&trampoline));

                if (result.has_value())
                {
                    PartShowSuppress::set_part_add_show_trampoline(trampoline);
                    logger.info("[dispatch] PartAddShow hook installed at 0x{:X}",
                                pasAddr);
                }
                else
                {
                    logger.warning("PartAddShow hook failed: {} — transition flash suppression disabled",
                                   DetourModKit::Hook::error_to_string(result.error()));
                }
            }
            else
            {
                logger.warning("[dispatch] PartAddShow AOB scan failed — transition flash suppression disabled");
            }
        }

        // Real-part scene-graph tear-down.
        if (!RealPartTearDown::resolve_helpers())
        {
            logger.warning(
                "[dispatch] tear_down: helper resolution failed — "
                "feature disabled");
        }

        // --- Input ---

        // Resolve WorldSystem pointer for player detection on load.
        {
            auto wsAddr = resolve_address(
                k_worldSystemCandidates,
                std::size(k_worldSystemCandidates),
                "WorldSystem");
            if (wsAddr)
            {
                world_system_ptr().store(wsAddr, std::memory_order_release);
                logger.info("WorldSystem pointer at 0x{:X}", wsAddr);
            }
            else
            {
                logger.warning("WorldSystem AOB failed — load-time transmog disabled");
            }
        }

        start_load_detect_thread();
        ensure_apply_worker_started();

        register_hotkeys();
        DMK::InputManager::get_instance().start();

        logger.info("Transmog initialization complete — SlotPopulator {}",
                    slot_populator_fn() ? "READY" : "UNAVAILABLE");

        return true;
    }

    void shutdown()
    {
        DMK::Logger::get_instance().info("{} shutting down...", MOD_NAME);
        shutdown_requested().store(true, std::memory_order_release);

        // Order is load-bearing: workers first, hooks second.
        //
        // Every worker we spawned calls raw game functions under SEH
        // (apply_all_transmog → SlotPopulator, debounce worker →
        // RealPartTearDown → safeTearDown). Joining them BEFORE
        // DMK_Shutdown removes the MinHook trampolines guarantees no
        // worker is mid-call into game code that the loader is about
        // to unmap.

        stop_load_detect_thread();
        stop_apply_worker();
        join_deferred_nametable_scan();

        DMK_Shutdown();
    }

} // namespace Transmog
