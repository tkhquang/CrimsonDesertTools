#include "transmog.hpp"
#include "aob_resolver.hpp"
#include "color_override/color_override.hpp"
#include "color_override/color_token_table.hpp"
#include "color_override/host_scope.hpp"
#include "dye_record_inject.hpp"
#include "color_override/setter_substitute.hpp"
#include "generated/dye_color_table.hpp"
#include "prefab_wrapper_swap.hpp"
#include "constants.hpp"
#include "indexed_string_table.hpp"
#include "input_handler.hpp"
#include "item_name_table.hpp"
#include "itemmesh_dumper.hpp"
#include "part_show_suppress.hpp"
#include "preset_manager.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "overlay.hpp"
#include "transmog_apply.hpp"
#include "transmog_hooks.hpp"
#include "transmog_map.hpp"
#include "transmog_worker.hpp"

#include <cdcore/controlled_char.hpp>
#include <cdcore/dmk_glue.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

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

        // When the dropdown is pinned to a non-controlled character,
        // route overlay-UI edits onto that character's body instead of
        // cross-applying onto whoever you control. Engine-triggered
        // equip events still target the controlled body, so the
        // controlled character's transmog stays consistent across
        // their own gear changes. Disable to restore the legacy
        // cross-body behaviour (preset items rendered on the
        // controlled body regardless of the dropdown).
        DMK::Config::register_atomic<bool>(
            "General", "ApplyToSelectedCharacter",
            "Apply To Selected Character",
            flag_apply_to_editing(), true);

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

        // Protagonist codename overrides for CDCore's appearance-config
        // classifier. Each codename is a substring search target inside
        // the actor's appearance-config asset path. Defaults match the
        // shipped engine subfolder names; provided in case a future
        // patch or mod renames a subfolder. Empty values are ignored.
        DMK::Config::register_string(
            "General", "KliffCodename", "Kliff Codename",
            [](const std::string &val)
            { CDCore::set_protagonist_codenames(val, {}, {}); },
            "cd_phm_macduff");
        DMK::Config::register_string(
            "General", "DamianeCodename", "Damiane Codename",
            [](const std::string &val)
            { CDCore::set_protagonist_codenames({}, val, {}); },
            "cd_phw_damian");
        DMK::Config::register_string(
            "General", "OongkaCodename", "Oongka Codename",
            [](const std::string &val)
            { CDCore::set_protagonist_codenames({}, {}, val); },
            "cd_phm_oongka");

        // Experimental: master toggle for the per-shader-property
        // ColorOverride pipeline (publisher hook, setter substitute,
        // host-scope owner-vfunc midhooks, and the per-region color
        // picker UI). Disabled by default; the feature relies on
        // AOB-resolved engine entry points that may shift under a
        // major game patch.
        DMK::Config::register_atomic<bool>(
            "Experimental", "ColorOverride", "Color Override",
            flag_color_override(), false);

        // One-shot diagnostic TSV dumps. Off by default; enable to
        // capture item-catalog / item->prefab snapshots once
        // ItemNameTable::build() lands. Both files are written to the
        // plugin's runtime directory.
        DMK::Config::register_atomic<bool>(
            "Diagnostics", "DumpItemPrefabsTsv", "Dump Item->Prefab TSV",
            flag_dump_item_prefabs(), false);
        DMK::Config::register_atomic<bool>(
            "Diagnostics", "DumpItemCatalogTsv", "Dump Item Catalog TSV",
            flag_dump_item_catalog(), false);

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

        PrefabWrapperSwap::register_config();

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

    namespace
    {
        // Map PresetManager character names to the 1-based char-idx
        // CDCore::snapshot_body_cache emits (1=Kliff, 2=Damiane,
        // 3=Oongka). Returns 0 for unknown names so the targeted-apply
        // path can refuse to schedule rather than guess.
        std::uint32_t char_idx_for_preset_name(
            const std::string &name) noexcept
        {
            if (name == "Kliff")   return 1;
            if (name == "Damiane") return 2;
            if (name == "Oongka")  return 3;
            return 0;
        }

        // Editing-target gate consulted by every overlay-UI entry point
        // (manual_apply, manual_apply_slot, manual_clear). Returns
        // true if the caller should proceed with `schedule_transmog_*`;
        // false if it should bail (the editing character is not in the
        // live snapshot and the user opted out of cross-body apply).
        //
        // When the pin is off OR the flag is off, the helper is a
        // no-op and the caller proceeds with the legacy controlled-
        // body path. When the pin is on AND the flag is on, the helper
        // resolves the editing character's char-idx, primes
        // `set_targeted_apply_char_idx` so the worker redirects this
        // apply, and returns true. If the editing character is not
        // currently live, the helper logs at info level and returns
        // false so the caller skips scheduling entirely.
        bool prime_targeted_apply_if_pinned() noexcept
        {
            auto &pm = PresetManager::instance();
            if (!pm.editing_pinned())
                return true;
            if (!flag_apply_to_editing().load(std::memory_order_acquire))
                return true;

            const std::string editName{pm.editing_character()};
            const auto idx = char_idx_for_preset_name(editName);
            if (idx == 0)
                return true; // Unknown character name; fall back to default.

            // Resolve the snapshot now so the entry point can decide
            // whether to schedule. The worker re-resolves at apply
            // time too -- this pre-check just lets us skip scheduling
            // when the editing body is clearly not live.
            std::array<CDCore::BodyCacheEntry, 3> entries{};
            const auto n = CDCore::snapshot_body_cache(
                entries.data(), entries.size());
            bool live = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (entries[i].charIdx == idx)
                {
                    live = true;
                    break;
                }
            }
            if (!live)
            {
                DMK::Logger::get_instance().info(
                    "[targeted-apply] editing '{}' not currently "
                    "loaded -- preset edit saved, render deferred "
                    "until {} is in the world",
                    editName, editName);
                return false;
            }

            set_targeted_apply_char_idx(idx);
            return true;
        }
    } // namespace

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
        if (!prime_targeted_apply_if_pinned())
            return;

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
        if (!prime_targeted_apply_if_pinned())
            return;

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
        if (!prime_targeted_apply_if_pinned())
            return;

        DMK::Logger::get_instance().info("Manual clear: scheduling (debounced)");
        clear_pending().store(true, std::memory_order_release);
        schedule_transmog_ms(k_manualDebounceMs);
    }

    // Walks the live auth-table entry array and snapshots each
    // LT-managed slot's dye records into the active preset's
    // SlotDyeChannels. The auth-table's per-entry dye-record vector
    // layout at entry+0x78 is:
    //
    //   +0x78  qword  data_ptr  -- heap address of contiguous
    //                              16-byte records
    //   +0x80  dword  count     -- number of valid records (0..N)
    //   +0x84  dword  capacity  -- allocation cap, ignored here
    //
    // Each 16-byte record matches DyeRecordInject::ChannelState:
    //   +0..3   group_hash
    //   +4..5   material_id
    //   +6      channel index (0..k_dyeChannelCount-1)
    //   +7..9   R / G / B
    //   +11     repair_byte
    //
    // This function is split out of capture_outfit() because it
    // mutates std::string members (ChannelDye::group_name), which
    // requires C++ object unwinding. MSVC's C2712 forbids that in
    // the same function as a __try frame, and capture_outfit() has
    // one. The caller's __try/__except therefore covers any access
    // fault from a stale auth table here too -- we deliberately do
    // NOT add another __try in this function.
    //
    // SAFETY: the raw reads of `entryArray`, `base + ...`, and the
    // call to read_entry_dye_records() can fault if the caller
    // hands us a stale or torn auth table. capture_outfit()'s
    // __try/__except is the only line of defence.
    // Copy a captured live-dye snapshot into a preset slot's per-
    // channel dye state. Skips channels with `group_hash == 0`,
    // resolves `group_name` from the static DyeColorTable so the
    // string-key survives a renumbered hash table across patches,
    // and flips `dyeSparse=true` so the apply path emits only the
    // channels the source item actually colours. Returns the count
    // of non-empty channels written. The caller owns any pre-wipe
    // of `slotPreset.dye[]` before invoking.
    static std::size_t apply_live_dye_to_preset_slot(
        PresetSlot &slotPreset,
        const DyeRecordInject::ChannelState (&live)
            [DyeRecordInject::k_dyeChannelCount]) noexcept
    {
        slotPreset.dyeSparse = true;
        std::size_t written = 0;
        for (std::size_t k = 0;
             k < DyeRecordInject::k_dyeChannelCount; ++k)
        {
            const auto &src = live[k];
            if (src.group_hash == 0)
                continue;
            auto &dst = slotPreset.dye[k];
            dst.group_hash  = src.group_hash;
            dst.r           = src.r;
            dst.g           = src.g;
            dst.b           = src.b;
            dst.material_id = src.material_id;
            dst.repair_byte = src.repair_byte;
            const auto *grp =
                DyeColorTable::find_group(src.group_hash);
            if (grp != nullptr && grp->string_key != nullptr)
                dst.group_name = grp->string_key;
            else
                dst.group_name.clear();
            ++written;
        }
        return written;
    }

    static void capture_live_dye_into_active_preset(
        uintptr_t entryArray, uint32_t entryCount) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        auto *activePreset =
            PresetManager::instance().active_preset_mut();
        if (activePreset == nullptr)
            return;

        // Wipe existing dye on every LT-managed slot before the
        // capture writes so channels that the captured item does
        // NOT override do not retain a stale value from a previous
        // capture or picker session.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (!Transmog::slot_enabled(i))
                continue;
            if (i >= activePreset->slots.size())
                continue;
            for (auto &ch : activePreset->slots[i].dye)
                ch = ChannelDye{};
        }

        bool any = false;
        for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
        {
            auto base = entryArray + e * k_compEntryStride;
            auto gameSlot =
                *reinterpret_cast<int16_t *>(base + k_compEntrySlotTagOffset);
            auto itemId =
                *reinterpret_cast<uint16_t *>(base + k_compEntryItemIdOffset);
            if (itemId == 0 || itemId == 0xFFFF)
                continue;
            auto tmSlot = slot_from_game_slot(gameSlot);
            if (!tmSlot.has_value())
                continue;
            auto idx = static_cast<std::size_t>(*tmSlot);
            if (!Transmog::slot_enabled(idx))
                continue;
            if (idx >= activePreset->slots.size())
                continue;

            DyeRecordInject::ChannelState
                live[DyeRecordInject::k_dyeChannelCount];
            const auto dyeFilled =
                DyeRecordInject::read_entry_dye_records(base, live);
            if (dyeFilled == 0)
                continue;

            DyeRecordInject::log_dye_snapshot(
                "capture",
                slot_name(*tmSlot),
                live);

            // Real-item capture -- the apply path emits sparse
            // records (only channels the item actually colours).
            // A slot previously edited in picker-dense mode also
            // flips back to sparse here.
            apply_live_dye_to_preset_slot(
                activePreset->slots[idx], live);
            any = true;
            logger.info(
                "    -> {} dye channel(s) captured for {}",
                dyeFilled, slot_name(*tmSlot));
        }

        if (any)
            dye_dirty().store(true, std::memory_order_release);
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

            namespace PWS = Transmog::PrefabWrapperSwap;

            // Capture is "snapshot what the user is currently
            // equipped with". Any session-only PWS prefab picks must
            // be cleared so the captured carrier itemIds become the
            // visible state -- otherwise the cyan prefab label would
            // hide the captured gear behind a stale prefab pick.
            // Disabled slots are skipped entirely (the dispatcher
            // won't service them; writing any captured state into
            // their mapping just bloats the in-memory rows the next
            // Save would already drop on disk per slot_metadata.hpp).
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (!Transmog::slot_enabled(i))
                    continue;
                const auto tslot = static_cast<TransmogSlot>(i);
                const int curSrc = PWS::selection_src_index(tslot);
                PWS::set_selection(tslot, curSrc, -1);
            }

            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (!Transmog::slot_enabled(i))
                    continue;
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
                    if (!Transmog::slot_enabled(idx))
                    {
                        logger.info(
                            "    -> Skipping {} (slot disabled)",
                            slot_name(*tmSlot));
                        continue;
                    }
                    slot_mappings()[idx].targetItemId = itemId;
                    ++captured;
                    logger.info("    -> Captured as {} target", slot_name(*tmSlot));
                }
            }

            // Live dye snapshot delegated to a separate function so
            // its std::string mutations don't conflict with this
            // function's __try frame (MSVC C2712 forbids C++ object
            // unwinding inside __try).
            capture_live_dye_into_active_preset(entryArray, entryCount);

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

        // Capture-style snapshot: skip disabled slots so we don't
        // bloat in-memory rows for slots the dispatcher won't service.
        // PWS picks are intentionally NOT preserved -- this function is
        // a "what is the user wearing right now" snapshot, mirroring
        // capture_outfit, and any session-only prefab pick should
        // surrender to the captured itemId so the visible state matches.
        namespace PWS = Transmog::PrefabWrapperSwap;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (!Transmog::slot_enabled(i))
                continue;
            const auto tslot = static_cast<TransmogSlot>(i);
            const int curSrc = PWS::selection_src_index(tslot);
            PWS::set_selection(tslot, curSrc, -1);
        }

        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (!Transmog::slot_enabled(i))
                continue;
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
                {
                    const auto idx = static_cast<std::size_t>(*tmSlot);
                    if (!Transmog::slot_enabled(idx))
                        continue;
                    slot_mappings()[idx].targetItemId = itemId;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("capture_real_equipment: access fault");
        }
    }

    // SEH-walk helper for sync_live_dye_for_slot. Split into its own
    // function because the caller mutates std::string members and
    // MSVC C2712 forbids __try in functions that require C++ object
    // unwinding. Returns 0 on miss or fault.
    static uintptr_t find_auth_entry_for_game_tag(
        __int64 a1, std::int16_t gameTag) noexcept
    {
        uintptr_t entryBase = 0;
        __try
        {
            const auto entryDesc =
                *reinterpret_cast<uintptr_t *>(a1 + k_compEntryTablePtrOffset);
            if (entryDesc < 0x10000)
                return 0;
            const auto entryArray =
                *reinterpret_cast<uintptr_t *>(entryDesc + 8);
            const auto entryCount =
                *reinterpret_cast<uint32_t *>(entryDesc + 16);
            for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
            {
                const auto base = entryArray + e * k_compEntryStride;
                const auto sl = *reinterpret_cast<int16_t *>(
                    base + k_compEntrySlotTagOffset);
                if (sl == gameTag)
                {
                    entryBase = base;
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return entryBase;
    }

    bool sync_live_dye_for_slot(std::size_t slotIdx) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        if (slotIdx >= k_slotCount)
            return false;
        if (!Transmog::slot_enabled(slotIdx))
        {
            logger.info("[dye-sync] slot {} disabled in mod config -- skipped",
                        slotIdx);
            return false;
        }

        auto *activePreset =
            PresetManager::instance().active_preset_mut();
        if (activePreset == nullptr)
            return false;
        if (slotIdx >= activePreset->slots.size())
            return false;

        const auto tslot = static_cast<TransmogSlot>(slotIdx);
        const auto gameTag = game_slot_from_transmog(tslot);

        const auto a1 = get_player_a1();
        if (a1 < 0x10000)
        {
            logger.info("[dye-sync] player not found -- skipped");
            return false;
        }

        const auto entryBase =
            find_auth_entry_for_game_tag(a1, gameTag);
        if (entryBase == 0)
        {
            logger.info("[dye-sync] no auth-table entry for slot {} "
                        "(gameTag={:#x}) -- skipped",
                        slot_name(tslot), gameTag);
            return false;
        }

        DyeRecordInject::ChannelState
            live[DyeRecordInject::k_dyeChannelCount];
        const auto dyeFilled =
            DyeRecordInject::read_entry_dye_records(entryBase, live);
        if (dyeFilled == 0)
        {
            logger.info("[dye-sync] slot {} has no live dye records -- "
                        "preset slot left untouched",
                        slot_name(tslot));
            return false;
        }

        DyeRecordInject::log_dye_snapshot(
            "sync", slot_name(tslot), live);

        // Wipe before writing so channels not present in `live` do
        // not linger from a prior picker session (matches the
        // capture_outfit per-slot pattern).
        auto &slotPreset = activePreset->slots[slotIdx];
        for (auto &ch : slotPreset.dye)
            ch = ChannelDye{};

        const bool any =
            apply_live_dye_to_preset_slot(slotPreset, live) > 0;

        if (any)
        {
            dye_dirty().store(true, std::memory_order_release);
            logger.info(
                "[dye-sync] slot {} captured {} channel(s) from live engine",
                slot_name(tslot), dyeFilled);
        }
        return any;
    }

    // --- Init / Shutdown ---

    bool init()
    {
        auto &logger = DMK::Logger::get_instance();

        // --- Required asset gate ---
        //
        // The display-names TSV ships with the mod and is required for
        // the catalog UI and name-keyed preset resolution. Missing file
        // means the user installed incorrectly; surface a hard, visible
        // error (modal popup) and bail out of init so the failure cannot
        // be silently ignored by skipping the log.
        {
            const auto rtDir = runtime_dir_utf8();
            std::string tsvPath =
                rtDir.empty() ? std::string{DISPLAY_NAMES_FILE}
                              : rtDir + DISPLAY_NAMES_FILE;
            const bool dirOk = !rtDir.empty();
            std::ifstream probe(tsvPath);
            if (!dirOk || !probe.is_open())
            {
                std::string body =
                    "Required asset '" + std::string{DISPLAY_NAMES_FILE} +
                    "' was not found.\n\nExpected location:\n  " + tsvPath +
                    "\n\nThe TSV must sit next to the mod DLL (same folder, "
                    "wherever you installed it). Reinstall the mod and "
                    "verify all files are present.\n\nThe mod will not "
                    "function.";
                logger.error("{}", body);
                ::MessageBoxA(nullptr, body.c_str(),
                              "CrimsonDesertLiveTransmog -- missing asset",
                              MB_OK | MB_ICONERROR | MB_TOPMOST |
                                  MB_SYSTEMMODAL);
                return false;
            }
        }

        // Apply config before the resolver and hook-install steps so
        // the INI LogLevel takes effect for any TRACE/DEBUG emissions
        // that follow. Setters dispatched by Config::load() touch only
        // atomics, preset/state structures, and InputManager bindings;
        // none of them depend on resolved addresses (those are
        // populated below).
        load_config();

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
            // Deferred slot-hash resolution. A synchronous scan here
            // would observe a small / empty IndexedStringA table on
            // cold-launch (LT loaded before the game finishes wiring
            // main-menu state), leaving PartShowSuppress inert for
            // the whole session. The deferred worker polls until
            // world-ready, then commits once every expected slot hash
            // is present. See transmog_worker.hpp for the contract.
            launch_deferred_slot_hash_scan();
            logger.info(
                "[dispatch] slot-hash resolution scheduled "
                "(deferred until world-ready)");
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
                if (flag_dump_item_catalog().load(std::memory_order_relaxed))
                    ItemNameTable::instance().dump_catalog_tsv();
                if (flag_dump_item_prefabs().load(std::memory_order_relaxed))
                {
                    // Targeted phantom-recovery sweep can take ~minutes
                    // on a cold registry. Detach so the rest of transmog
                    // init (hooks, color-override) doesn't block waiting
                    // for the TSV write -- nothing downstream depends on
                    // the dump.
                    std::thread{[] { dump_itemmesh_tsv(); }}.detach();
                }
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

            if (iseAddr && !DMK::Scanner::is_likely_function_prologue(iseAddr))
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
            if (DMK::Scanner::is_likely_function_prologue(addrs.slotPopulator))
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

            if (beAddr && !DMK::Scanner::is_likely_function_prologue(beAddr))
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
            if (vecAddr && !DMK::Scanner::is_likely_function_prologue(vecAddr))
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

            if (pasAddr && !DMK::Scanner::is_likely_function_prologue(pasAddr))
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

        // Resolve WorldSystem pointer for LT-local chain walks
        // (per-character presets, load-detect, apply-side a1 fallbacks).
        // Independent of CDCore::controlled_char, which now uses the
        // static-chain [moduleBase + 0x5FA0430] anchor and does not
        // need a published WorldSystem holder.
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
                logger.warning(
                    "WorldSystem AOB failed -- load-time transmog and "
                    "per-character presets disabled");
            }
        }

        PrefabWrapperSwap::init();

        // Per-slot dye-record injector. Hooks the engine's dye-copier
        // primitive and appends fabricated ARMOR_MOD records so fake
        // transmog items render with user-chosen colors regardless of
        // the underlying real item.
        DyeRecordInject::init();

        // ColorOverride is a tri-hook subsystem (host-scope owner
        // vfuncs, setter property substitute, publisher per-matInst
        // capture). Gated behind the `[Experimental] ColorOverride`
        // INI key so the hooks don't install on the default
        // configuration; the picker UI keys off the same flag.
        if (flag_color_override().load(std::memory_order_acquire))
        {
            ColorOverride::HostScope::init();
            ColorOverride::SetterSubstitute::init();
            Transmog::ColorOverride::init();
        }
        else
        {
            DMK::Logger::get_instance().info(
                "[color-override] disabled by [Experimental] "
                "ColorOverride=false; subsystem skipped");
        }

        // Crimson Desert has TWO independent dye layers:
        //   1. Bench/menu UI dyeability -- gated by the
        //      partprefabdyeslotinfo.pabgb registry. Not modified at
        //      runtime; static PAZ overlays handle this externally.
        //   2. Render-time dye apply -- engine reads dye records from
        //      a publish vector at dst+120 during slotpop. The
        //      DyeRecordInject inline detour on sub_141E019E0 (init
        //      above) injects user-chosen records here.

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
        join_deferred_slot_hash_scan();

        PrefabWrapperSwap::shutdown();

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
