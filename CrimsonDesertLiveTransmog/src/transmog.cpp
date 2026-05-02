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
#include "overlay.hpp"
#include "transmog_apply.hpp"
#include "transmog_hooks.hpp"
#include "transmog_map.hpp"
#include "transmog_worker.hpp"

#include <cdcore/controlled_char.hpp>
#include <cdcore/dmk_glue.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <string>

namespace Transmog
{
    // --- Config ---

    static void load_config()
    {
        DMK::Config::register_log_level(
            "General", "LogLevel", "INFO");

        DMK::Config::register_atomic<bool>(
            "General", "Enabled", "Enabled", flag_enabled(), true);

        DMK::Config::register_atomic<bool>(
            "General", "PlayerOnly", "Player Only", flag_player_only(), true);

        // When true, always use the standalone transparent overlay window
        // instead of the ReShade addon tab. Useful if ReShade is installed
        // but the user prefers the standalone overlay.
        DMK::Config::register_bool(
            "General", "ForceStandaloneOverlay", "Force Standalone Overlay",
            [](bool val)
            {
                set_force_standalone(val);
            },
            false);

        // Auto-reload toggle. Off-by-default would force a relaunch for
        // every INI tweak; on-by-default keeps the iteration loop tight.
        // Setters invoked from the watcher thread are idempotent (every
        // register_atomic / register_press_combo path is safe to
        // re-fire).
        static std::atomic<bool> s_autoReload{true};
        DMK::Config::register_atomic<bool>(
            "General", "AutoReloadConfig", "Auto-Reload Config",
            s_autoReload, true);

        // Hotkey bindings are registered here, while the Config registry
        // is being populated, so the press_combo INI keys participate in
        // the same Config::load() pass below. register_press_combo also
        // ties each binding to InputManager directly; we just need to
        // ensure that InputManager::start() runs after this call (handled
        // in init() further down).
        register_hotkeys();

        DMK::Config::load(INI_FILE);
        DMK::Config::log_all();

        if (s_autoReload.load(std::memory_order_relaxed))
        {
            // Atomic flags update silently through their per-setter
            // callbacks; the watcher does not drive any game-state
            // work. Re-applying or clearing transmog still requires
            // a hotkey or in-game action.
            const auto status = DMK::Config::enable_auto_reload(
                std::chrono::milliseconds{250},
                [](bool content_changed)
                {
                    auto &logger = DMK::Logger::get_instance();
                    if (content_changed)
                        logger.info("INI auto-reload: setters applied");
                    else
                        logger.info("INI auto-reload: skipped (no content delta)");
                });
            if (status != DMK::Config::AutoReloadStatus::Started &&
                status != DMK::Config::AutoReloadStatus::AlreadyRunning)
            {
                DMK::Logger::get_instance().warning(
                    "INI auto-reload could not start (status enum {})",
                    static_cast<int>(status));
            }
        }
    }

    // --- Player-component layout ---
    //
    // Pointer to the PartDef/auth-table container on the SlotPopulator
    // descriptor (a1). The container is the entry table used by the
    // capture path:
    //
    //   container +0x08  QWORD   entry array base
    //   container +0x10  DWORD   live entry count
    //   container +0x14  DWORD   capacity
    //
    // The pointer slot itself shifted between game versions as the
    // component grew new fields below it:
    //
    //   v1.03.01 -- pointer @ a1 + 0x78
    //   v1.04.00 -- pointer @ a1 + 0x88   (+0x10 of new fields inserted)
    //   v1.05.00 -- pointer @ a1 + 0x88   (unchanged from v1.04)
    //
    // Reading the old offset on v1.04.00 returns garbage (the qword
    // that lives at +0x78 is no longer the container pointer), which
    // is what produced the "Capture: no entry table" warning. Mirrors
    // k_containerPtrOffset in real_part_tear_down.cpp.
    constexpr std::ptrdiff_t k_compEntryTablePtrOffset = 0x88;

    // Entry layout within the auth-table array. v1.05 grew the entry
    // by 8 bytes and shifted the slot tag accordingly:
    //
    //   v1.04.00: stride=0xC8 (200), slotTag@+0xC0
    //   v1.05.00: stride=0xD0 (208), slotTag@+0xC8
    //
    // primary item id at +0x08 is unchanged across versions. Slot-tag
    // VALUES themselves are unchanged (Helm=0x03, Chest=0x04, Gloves=0x05,
    // Boots=0x06, Cloak=0x10); only the position within the entry shifted.
    // Mirrors k_entryStride / k_entrySlotTagOffset in real_part_tear_down.cpp.
    constexpr std::ptrdiff_t k_compEntryStride         = 0xD0;
    constexpr std::ptrdiff_t k_compEntryItemIdOffset   = 0x08;
    constexpr std::ptrdiff_t k_compEntrySlotTagOffset  = 0xC8;

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
        if (!slot_populator_fn())
        {
            DMK::Logger::get_instance().debug("Manual apply: SlotPopulator not resolved (AOB failed)");
            return;
        }
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
        if (!slot_populator_fn())
            return;
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
        if (!slot_populator_fn())
            return;
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
        if (!slot_populator_fn())
            return;
        auto &logger = DMK::Logger::get_instance();
        auto a1 = get_player_a1();
        if (a1 < 0x10000)
        {
            logger.info("Capture: player not found");
            return;
        }

        __try
        {
            auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + k_compEntryTablePtrOffset);
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
                auto base = entryArray + e * k_compEntryStride;
                auto gameSlot = *reinterpret_cast<int16_t *>(base + k_compEntrySlotTagOffset);
                auto itemId = *reinterpret_cast<uint16_t *>(base + k_compEntryItemIdOffset);

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
            auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + k_compEntryTablePtrOffset);
            if (entryDesc < 0x10000) return;
            auto entryArray = *reinterpret_cast<uintptr_t *>(entryDesc + 8);
            auto entryCount = *reinterpret_cast<uint32_t *>(entryDesc + 16);

            for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
            {
                auto base = entryArray + e * k_compEntryStride;
                auto gameSlot = *reinterpret_cast<int16_t *>(base + k_compEntrySlotTagOffset);
                auto itemId = *reinterpret_cast<uint16_t *>(base + k_compEntryItemIdOffset);

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

        if (!DMK::Memory::init_cache())
            logger.warning("Memory cache init failed -- pointer reads may be slower");

        // --- Resolve AOB addresses ---
        //
        // The game may restart its exe during shader compilation.  If
        // we load before the code section is fully populated, all scans
        // fail.  Retry up to 5 times with a 3-second delay if the
        // critical SlotPopulator pattern isn't found.

        auto &addrs = resolved_addrs();

        // SlotPopulator: the KEY function for transmog.
        addrs.slotPopulator = resolve_address(
            k_slotPopulatorCandidates,
            std::size(k_slotPopulatorCandidates),
            "SlotPopulator");

        if (!addrs.slotPopulator)
            logger.warning("SlotPopulator AOB scan failed -- transmog will not work");

        // MapLookup: IndexedStringA::lookup. Not hooked -- RIP anchor for
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
                    "[dispatch] IndexedStringA scan returned empty -- "
                    "slot hashes unresolved, PartShowSuppress disabled");
            }
        }
        else
        {
            logger.warning(
                "MapLookup AOB scan failed -- cannot resolve CD_* slot hashes, "
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
                // Load display names BEFORE dump_catalog_tsv so the
                // sorted cache (built lazily by the dump) already
                // contains display names and doesn't need a second
                // rebuild that would stall the overlay render thread.
                {
                    const auto dir = runtime_dir_utf8();
                    if (!dir.empty())
                        ItemNameTable::instance().load_display_names(
                            dir + DISPLAY_NAMES_FILE);
                }
                if (logger.is_enabled(DMK::LogLevel::Trace))
                    ItemNameTable::instance().dump_catalog_tsv();
            }
            else if (result == BR::Deferred)
            {
                logger.info(
                    "[nametable] iteminfo global not initialized yet -- "
                    "starting background scan thread");
                launch_deferred_nametable_scan();
            }
            else // Fatal
            {
                logger.warning(
                    "[nametable] address chain resolution failed -- "
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
                "SubTranslator AOB scan failed -- cannot build item-name table, "
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
                "SafeTearDown AOB scan failed -- real_part_tear_down will "
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
                    "looks wrong -- rejecting",
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
                    "InitSwapEntry AOB scan failed -- transmog apply will "
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
            const auto rtDir = runtime_dir_utf8();
            const std::string presetsPath = rtDir + PRESETS_FILE;

            auto &pm = PresetManager::instance();
            pm.load(presetsPath);
            pm.apply_to_state();
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
                    "looks wrong -- rejecting, transmog apply disabled",
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
                    "looks wrong -- rejecting",
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
                logger.warning("BatchEquip AOB scan failed -- transmog equip trigger disabled");
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
                    "wrong -- rejecting",
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

        // PartAddShow (sub_14081DC20) inline hook -- transition-flash polish.
        //
        // Auto-skip if CrimsonDesertEquipHide is loaded in-process.
        // EH installs its own inline hook on the exact same function
        // for its gliding-fix. Two inline hooks on one address chain
        // non-deterministically across game launches (DLL load order
        // isn't fixed), and one side can end up silently bypassed.
        // LT's hook is cosmetic polish, EH's is user-visible
        // functionality -- when both are present, yield to EH.
        //
        // DMK 3.2.2 ships HookManager::is_target_already_hooked() for the
        // managed-hook-vs-managed-hook collision case, but the substring
        // module-name match here covers the orthogonal scenario where EH
        // has been loaded but has not yet installed its hook (load-order
        // race during the worker thread's init pass) or hooks via a
        // non-DMK route. Yielding on module presence avoids the race
        // entirely. The substring match covers both the dev two-DLL
        // ("..._Logic.dll") and the release single-ASI (".asi") layouts
        // in one call.
        const bool ehPresent =
            CDCore::Glue::is_sibling_mod_loaded("CrimsonDesertEquipHide");
        if (ehPresent)
        {
            logger.info("[dispatch] PartAddShow hook skipped -- "
                        "CrimsonDesertEquipHide detected; yielding to "
                        "its gliding-fix hook to avoid dual-install "
                        "ordering issues.");
        }
        else
        {
            auto pasAddr = resolve_address(
                k_partAddShowCandidates,
                std::size(k_partAddShowCandidates),
                "PartAddShow");

            if (pasAddr && !sanity_check_function_prologue(pasAddr))
            {
                logger.warning(
                    "PartAddShow resolved to 0x{:X} but prologue byte "
                    "looks wrong -- rejecting",
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
                    logger.warning("PartAddShow hook failed: {} -- transition flash suppression disabled",
                                   DetourModKit::Hook::error_to_string(result.error()));
                }
            }
            else
            {
                logger.warning("[dispatch] PartAddShow AOB scan failed -- transition flash suppression disabled");
            }
        }

        // Real-part scene-graph tear-down.
        if (!RealPartTearDown::resolve_helpers())
        {
            logger.warning(
                "[dispatch] tear_down: helper resolution failed -- "
                "feature disabled");
        }

        // --- Input ---

        // Resolve WorldSystem pointer for player detection on load and
        // publish the same address to Core. The WorldSystem chain is the
        // client-side reach used by load-time transmog and the per-frame
        // body-pool walk; on v1.04.00 it no longer participates in
        // controlled-character identification (the +0x50 byte that used
        // to discriminate between protagonists ceased to be unique in
        // this build). Identity decoding now consumes the player static
        // resolved below.
        {
            auto wsAddr = resolve_address(
                k_worldSystemCandidates,
                std::size(k_worldSystemCandidates),
                "WorldSystem");
            if (wsAddr)
            {
                world_system_ptr().store(wsAddr, std::memory_order_release);
                logger.info("WorldSystem pointer at 0x{:X}", wsAddr);
                CDCore::set_world_system_holder(wsAddr);
            }
            else
            {
                logger.warning(
                    "WorldSystem AOB failed -- load-time transmog and "
                    "per-character presets disabled");
            }
        }

        // Resolve the server-side player static and publish it to Core.
        // The cascade exposes a RIP-relative load-or-store of the static
        // qword (writer in P1, two readers in P2/P3); the resolver walks
        // root -> NwVirtualAsyncSession (+0x18) -> ServerUserActor (+0xA0)
        // -> ServerChildOnlyInGameActor (+0xD0) and identifies the
        // controlled character by which fixed-offset slot inside the
        // party container has slot+0x2C set to 1. An unresolved holder
        // degrades the resolver to Unknown without crashing; consumer
        // code already treats Unknown as "preserve previous identity".
        {
            auto playerAddr = resolve_address(
                k_playerStaticCandidates,
                std::size(k_playerStaticCandidates),
                "PlayerStatic");
            if (playerAddr)
            {
                logger.info("Player static at 0x{:X}", playerAddr);
                CDCore::set_player_static_holder(playerAddr);
            }
            else
            {
                logger.warning(
                    "PlayerStatic AOB failed -- controlled-character "
                    "detection disabled, presets cannot route by character");
            }
        }

        // Resolve the radial-UI swap-key capture site and install the
        // mid-hook in Core. The hook stamps a pending characterinfo key
        // (1 = Kliff, 4 = Damiane, 6 = Oongka) every time the radial UI
        // commits a swap; the resolver consumes that key on the next
        // chain walk to bind the live userActor pointer to a known
        // character, providing deterministic identity for ghost-roster
        // states the slot-offset decode cannot resolve. Optional
        // feature -- failure leaves the slot-offset decode and the LKG
        // cache as the resolver's only layers, which is the pre-fix
        // behaviour.
        //
        // Two-consumer (LT + EH) coordination: CrimsonDesertCore is a
        // STATIC library, so this DLL owns its own MidHook independent
        // of any sibling DLL's MidHook against the same process address.
        // The AOB cascade is ordered to match either an unpatched site
        // or a site whose prelude bytes have already been displaced by
        // a sibling's earlier MidHook install (see anchors.hpp), so the
        // scan succeeds regardless of load order. SafetyHook's internal
        // chaining at the JMP target lets both DLLs' callbacks fire on
        // every radial swap.
        {
            const auto radialAddr = resolve_address(
                k_radialSwapKeyCandidates,
                std::size(k_radialSwapKeyCandidates),
                "RadialSwapKey");
            if (radialAddr)
            {
                if (!CDCore::install_radial_swap_hook(radialAddr))
                {
                    logger.warning(
                        "Radial-swap key hook install failed -- "
                        "ghost-roster character resolution disabled");
                }
            }
            else
            {
                logger.warning(
                    "RadialSwapKey AOB failed -- ghost-roster "
                    "character resolution disabled");
            }
        }

        start_load_detect_thread();
        ensure_apply_worker_started();

        // Hotkey bindings were registered in load_config() so the
        // press_combo INI keys could be picked up during Config::load().
        // Now flip InputManager live; the bindings start firing on the
        // next poll tick.
        DMK::InputManager::get_instance().start();

        logger.info("Transmog initialization complete -- SlotPopulator {}",
                    slot_populator_fn() ? "READY" : "UNAVAILABLE");

        return true;
    }

    void shutdown()
    {
        DMK::Logger::get_instance().info("{} shutting down...", MOD_NAME);

        // Disable the INI watcher up front so an in-flight save event
        // cannot fire setters while we are tearing state down.
        DMK::Config::disable_auto_reload();

        shutdown_requested().store(true, std::memory_order_release);

        // Drain workers before the DMK teardown removes the hooks they
        // call into. Every worker we spawned calls raw game functions
        // under SEH (apply_all_transmog -> SlotPopulator, debounce
        // worker -> RealPartTearDown -> safeTearDown), so joining them
        // first guarantees no worker is mid-call into a SafetyHook
        // trampoline when the trampoline pages are unmapped.
        stop_load_detect_thread();
        stop_apply_worker();
        join_deferred_nametable_scan();

        // Tear down the Core-owned radial-swap mid-hook before the
        // SafetyHook trampoline pages can be touched by a late-firing
        // game thread. Idempotent: safe to call when no hook is
        // installed and when the matching uninstall has already run
        // from a sibling consumer.
        CDCore::uninstall_radial_swap_hook();

        // Full DMK teardown: removes every managed hook (BatchEquip,
        // VEC, PartAddShow), stops and clears the InputManager poller
        // along with its registered bindings, stops the ConfigWatcher,
        // and clears the Config registered-items list. Idempotent and
        // safe to re-init from on the next Logic-DLL load. Each detour
        // body snapshots its trampoline pointer at entry and bails to
        // a benign default if the snapshot is null, defending the
        // brief drain window between hook removal and DLL unmap.
        DMK_Shutdown();

        clear_hotkey_guards();
    }

} // namespace Transmog
