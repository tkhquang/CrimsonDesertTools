#include "transmog.hpp"
#include "aob_resolver.hpp"
#include "auth_table.hpp"
#include "color_override/color_override.hpp"
#include "color_override/color_token_discovery.hpp"
#include "color_override/color_token_table.hpp"
#include "color_override/host_scope.hpp"
#include "dye_record_inject.hpp"
#include "color_override/setter_substitute.hpp"
#include "generated/dye_color_table.hpp"
#include "claim_walk_guard.hpp"
#include "socket_mesh_override.hpp"
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
#include "transmog_map.hpp"
#include "transmog_worker.hpp"
#include "helm_audio_filter.hpp"

#include <cdcore/anchors.hpp>
#include <cdcore/controlled_char.hpp>

#include <DetourModKit/abi/wheel_host.h>
#include <DetourModKit/config.hpp>
#include <DetourModKit/diagnostics.hpp>
#include <DetourModKit/error.hpp>
#include <DetourModKit/filesystem.hpp>
#include <DetourModKit/format.hpp>
#include <DetourModKit/hook.hpp>
#include <DetourModKit/input.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/scan.hpp>
#include <DetourModKit/session.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

namespace Transmog
{
    // Every hook the mod installs, from every feature module. A HookStack restores newest first, the only safe order
    // for layered hooks on one target. An older layer's restore otherwise clobbers a prologue a newer layer's live
    // trampoline still chains through. Each installer takes it by reference and pushes what it armed, so the whole
    // mod tears down through one clear() in shutdown(), while the code pages are still mapped.
    static DMK::hook::HookStack s_hooks;

    // EquipHide's module name, used to yield the shared PartAddShow target to it. Both of EquipHide's shipped shapes
    // - the release single ASI and the dev resident loader that stages a logic DLL - load under this exact name.
    inline constexpr std::string_view EQUIP_HIDE_MODULE = "CrimsonDesertEquipHide.asi";

    // Config

    static void load_config(DMK::Session &session)
    {
        // Section-scoped binders, so each INI section name is written once here instead of heading every call.
        const DMK::config::SectionBinder general = DMK::config::section("General");
        const DMK::config::SectionBinder experimental = DMK::config::section("Experimental");
        const DMK::config::SectionBinder diagnostics = DMK::config::section("Diagnostics");
        const DMK::config::SectionBinder advanced = DMK::config::section("Advanced");

        general.bind_log_level("LogLevel", "INFO");

        general.bind<bool>("Enabled", "Enabled", flag_enabled(), true);

        general.bind<bool>("PlayerOnly", "Player Only", flag_player_only(), true);

        // When the dropdown is pinned to a non-controlled character, route overlay-UI edits onto that character's body
        // instead of cross-applying onto whoever you control. Engine-triggered equip events still target the
        // controlled body, so the controlled character's transmog stays consistent across their own gear changes.
        // Disable to render preset items on the controlled body regardless of the dropdown.
        general.bind<bool>("ApplyToSelectedCharacter", "Apply To Selected Character", flag_apply_to_editing(), true);

        // Advanced: rtti_dissect self-heal search radius (bytes, per side) for the manager->user_actor offset recovery
        // in CDCore. The default 0x200 covers roughly ten times the worst drift seen so far. Raise it toward
        // MAX_HEAL_WINDOW only if a game patch pushes the field further. Not for normal users.
        advanced.bind<int>("SelfHealWindow", "Self Heal Window", CDCore::heal_window_setting(), 0x200);

        // When true, always use the standalone transparent overlay window instead of the ReShade addon tab. Useful if
        // ReShade is installed but the user prefers the standalone overlay.
        general.bind_bool(
            "ForceStandaloneOverlay",
            "Force Standalone Overlay",
            [](bool val) { set_force_standalone(val); },
            false
        );

        // Protagonist codename overrides for CDCore's appearance-config classifier. Each codename is a substring
        // search target inside the actor's appearance-config asset path. The defaults match the shipped engine
        // subfolder names. The overrides exist in case a future patch or mod renames a subfolder. Empty values are
        // ignored.
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

        // Experimental: master toggle for the per-shader-property color_override pipeline (publisher hook, setter
        // substitute, host-scope owner-vfunc midhooks, and the per-region color picker UI). Disabled by default. The
        // feature relies on AOB-resolved engine entry points that can shift under a major game patch.
        experimental.bind<bool>("ColorOverride", "Color Override", flag_color_override(), false);

        // Experimental: helm voice-unmuffle filter. Disabled by default. Enable it to remove the engine's stock
        // plate/heavy-helm voice muffle on protagonists. The hook installs at startup only when this is true, so a
        // toggle change takes effect on the next game launch. NPC voice muffle is unaffected either way.
        experimental.bind<bool>("UnmuffleHelmVoice", "Unmuffle Helm Voice", flag_helm_audio_unmuffle(), false);

        // One-shot diagnostic TSV dumps. Off by default. Enable them to capture item-catalog and item->prefab
        // snapshots after ItemNameTable::build() lands. Both files are written to the plugin's runtime directory.
        diagnostics.bind<bool>("DumpItemPrefabsTsv", "Dump Item->Prefab TSV", flag_dump_item_prefabs(), false);
        diagnostics.bind<bool>("DumpItemCatalogTsv", "Dump Item Catalog TSV", flag_dump_item_catalog(), false);

        // Auto-reload toggle. An off-by-default watcher forces a relaunch for every INI tweak. On-by-default keeps the
        // iteration loop tight. Setters invoked from the watcher thread are idempotent (every bind and press_combo
        // path is safe to re-fire).
        static std::atomic<bool> s_auto_reload{true};
        general.bind<bool>("AutoReloadConfig", "Auto-Reload Config", s_auto_reload, true);

        // load_config registers each hotkey binding here, while the config registry fills, so the press_combo INI
        // keys join the same load pass below. press_combo also registers each binding with the input engine, so
        // Input::start() must run after this call (handled in init() further down). The returned guards go into the
        // Session's input scope, which ~Session clears first and in reverse insertion order.
        register_hotkeys(session.scope());

        prefab_wrapper_swap::register_config();

        session.ini().load(INI_FILE);
        DMK::config::log_all();

        if (s_auto_reload.load(std::memory_order_relaxed))
        {
            // Atomic flags update silently through their per-setter callbacks. The watcher does not drive any
            // game-state work. A re-apply or a clear of transmog still requires a hotkey or an in-game action.
            const auto status = DMK::config::enable_auto_reload(
                std::chrono::milliseconds{250},
                [](bool content_changed)
                {
                    auto &logger = DMK::log();
                    if (content_changed)
                        logger.info("INI auto-reload: setters applied");
                    else
                        logger.info("INI auto-reload: skipped (no content delta)");
                }
            );
            if (status != DMK::config::AutoReloadStatus::Started &&
                status != DMK::config::AutoReloadStatus::AlreadyRunning)
            {
                DMK::log().warning("INI auto-reload could not start (status enum {})", static_cast<int>(status));
            }
        }
    }

    // Player-component layout
    //
    // Auth-table geometry (container pointer, entry stride, field offsets) lives in auth_table.hpp - one copy for
    // the whole mod, because the whole struct moves as a unit on patch day.

    // Public interface

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
        // Editing-target gate consulted by every overlay-UI entry point (manual_apply, manual_apply_slot,
        // manual_clear). It returns true when the caller can proceed with `schedule_transmog_*`. It returns false when
        // the caller must bail, because the editing character is not in the live snapshot and the user opted out of
        // cross-body apply.
        //
        // When the pin is off OR the flag is off, the helper is a no-op and the caller proceeds against the
        // controlled body. When the pin is on AND the flag is on, the helper resolves the editing character's
        // char-idx, primes `set_targeted_apply_char_idx` so the worker redirects this apply, and returns true. If the
        // editing character is not live, the helper logs at info level and returns false, so the caller skips the
        // schedule step.
        bool prime_targeted_apply_if_pinned() noexcept
        {
            auto &pm = PresetManager::instance();
            if (!pm.editing_pinned())
                return true;
            if (!flag_apply_to_editing().load(std::memory_order_acquire))
                return true;

            const std::string edit_name{pm.editing_character()};
            const auto idx = CDCore::character_idx_from_name(edit_name);
            if (idx == 0)
                return true; // Unknown character name; fall back to default.

            // Resolve the snapshot now so the entry point can decide whether to schedule. The worker re-resolves at
            // apply time too. This pre-check only skips the scheduling step when the editing body is not live.
            std::array<CDCore::BodyCacheEntry, 3> entries{};
            const auto n = CDCore::snapshot_body_cache(entries.data(), entries.size());
            bool live = false;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (entries[i].char_idx == idx)
                {
                    live = true;
                    break;
                }
            }
            if (!live)
            {
                DMK::log().info(
                    "[targeted-apply] editing '{}' not currently "
                    "loaded - preset edit saved, render deferred until {} is in the world",
                    edit_name,
                    edit_name
                );
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
            DMK::log().debug("Manual apply: SlotPopulator not resolved (AOB failed)");
            return;
        }
        if (!is_world_ready())
        {
            DMK::log().debug("Manual apply: player not found");
            return;
        }
        if (!prime_targeted_apply_if_pinned())
            return;

        DMK::log().debug("Manual apply: scheduling (debounced)");
        clear_pending().store(false, std::memory_order_release);
        // Reset to "all slots" so the worker runs the full path.
        pending_slot_index().store(SLOT_COUNT, std::memory_order_release);
        schedule_transmog_ms(MANUAL_DEBOUNCE_MS);
    }

    void manual_apply_slot(std::size_t slot_idx)
    {
        if (!slot_populator_fn())
            return;
        if (!is_world_ready())
        {
            DMK::log().debug("Manual apply slot={}: player not found", slot_idx);
            return;
        }
        if (!prime_targeted_apply_if_pinned())
            return;

        DMK::log().debug("Manual apply slot={}: scheduling (debounced)", slot_idx);
        clear_pending().store(false, std::memory_order_release);
        pending_slot_index().store(slot_idx, std::memory_order_release);
        schedule_transmog_ms(MANUAL_DEBOUNCE_MS);
    }

    void manual_clear()
    {
        if (!slot_populator_fn())
            return;
        if (!is_world_ready())
        {
            DMK::log().warning("Manual clear: player not found");
            return;
        }
        if (!prime_targeted_apply_if_pinned())
            return;

        DMK::log().info("Manual clear: scheduling (debounced)");
        clear_pending().store(true, std::memory_order_release);
        schedule_transmog_ms(MANUAL_DEBOUNCE_MS);
    }

    // Copies a captured live-dye snapshot into a preset slot's per-channel dye state. It skips channels with
    // `group_hash == 0`. It resolves `group_name` from the static dye_color_table, so the string key survives a
    // renumbered hash table across patches. It sets `dye_sparse=true`, so the apply path emits only the channels that
    // the source item actually colors. It returns the count of non-empty channels written. The caller owns any
    // pre-wipe of `slot_preset.dye[]` before the call.
    //
    // This work lives outside capture_outfit() because it mutates std::string members (ChannelDye::group_name), which
    // requires C++ object unwinding. MSVC C2712 forbids that in the same function as a __try frame, and
    // capture_outfit() owns one. Keep this function out of every __try frame.
    static std::size_t apply_live_dye_to_preset_slot(
        PresetSlot &slot_preset,
        const dye_record_inject::ChannelState (&live)[dye_record_inject::DYE_CHANNEL_COUNT]
    ) noexcept
    {
        slot_preset.dye_sparse = true;
        std::size_t written = 0;
        for (std::size_t k = 0; k < dye_record_inject::DYE_CHANNEL_COUNT; ++k)
        {
            const auto &src = live[k];
            if (src.group_hash == 0)
                continue;
            auto &dst = slot_preset.dye[k];
            dst.group_hash = src.group_hash;
            dst.r = src.r;
            dst.g = src.g;
            dst.b = src.b;
            dst.material_id = src.material_id;
            dst.repair_byte = src.repair_byte;
            const auto *grp = dye_color_table::find_group(src.group_hash);
            if (grp != nullptr && grp->string_key != nullptr)
                dst.group_name = grp->string_key;
            else
                dst.group_name.clear();
            ++written;
        }
        return written;
    }

    // Walks the live auth-table entry array and snapshots each LT-managed slot's dye records into the active preset's
    // SlotDyeChannels through read_entry_dye_records(). The per-entry dye-record vector sits inside the auth entry, so
    // its offset moves together with the auth-table entry geometry. The vector header and the 16-byte record layout
    // are declared once in dye_record_inject.hpp (DYE_VECTOR_OFFSET / VEC_DATA_OFFSET / VEC_COUNT_OFFSET, and the
    // field map on ChannelState). This function reaches them only through read_entry_dye_records(), so the offsets
    // are deliberately NOT restated here.
    //
    // This function is split out of capture_outfit() for the same MSVC C2712 reason as apply_live_dye_to_preset_slot
    // above. The caller's __try/__except therefore covers any access fault from a stale auth table here too, and this
    // function deliberately does NOT open another __try frame.
    //
    // SAFETY: the raw reads of `entry_array`, `base + ...`, and the call to read_entry_dye_records() can fault when the
    // caller passes a stale or torn auth table. capture_outfit()'s __try/__except is the only line of defense.
    static void capture_live_dye_into_active_preset(uintptr_t entry_array, uint32_t entryCount) noexcept
    {
        auto &logger = DMK::log();
        auto *active_preset = PresetManager::instance().active_preset_mut();
        if (active_preset == nullptr)
            return;

        // Wipe existing dye on every LT-managed slot before the capture writes so channels that the captured item does
        // NOT override do not retain a stale value from a previous capture or picker session.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (!Transmog::slot_enabled(i))
                continue;
            if (i >= active_preset->slots.size())
                continue;
            for (auto &ch : active_preset->slots[i].dye)
                ch = ChannelDye{};
        }

        bool any = false;
        for (uint32_t e = 0; e < entryCount && entry_array > 0x10000; ++e)
        {
            const auto base = entry_array + e * auth_table::ENTRY_STRIDE;
            const auto game_slot = *reinterpret_cast<int16_t *>(base + auth_table::ENTRY_SLOT_TAG_OFFSET);
            const auto item_id = *reinterpret_cast<uint16_t *>(base + auth_table::ENTRY_ITEM_ID_OFFSET);
            if (item_id == 0 || item_id == 0xFFFF)
                continue;
            const auto tm_slot = slot_from_game_slot(game_slot);
            if (!tm_slot.has_value())
                continue;
            const auto idx = static_cast<std::size_t>(*tm_slot);
            if (!Transmog::slot_enabled(idx))
                continue;
            if (idx >= active_preset->slots.size())
                continue;

            dye_record_inject::ChannelState live[dye_record_inject::DYE_CHANNEL_COUNT];
            const auto dye_filled = dye_record_inject::read_entry_dye_records(base, live);
            if (dye_filled == 0)
                continue;

            dye_record_inject::log_dye_snapshot("capture", slot_name(*tm_slot), live);

            // Real-item capture. The apply path emits sparse records, that is, only the channels the item actually
            // colors. A slot previously edited in picker-dense mode also flips back to sparse here.
            apply_live_dye_to_preset_slot(active_preset->slots[idx], live);
            any = true;
            logger.info("    -> {} dye channel(s) captured for {}", dye_filled, slot_name(*tm_slot));
        }

        if (any)
            dye_dirty().store(true, std::memory_order_release);
    }

    void capture_outfit()
    {
        if (!slot_populator_fn())
            return;
        auto &logger = DMK::log();
        const auto a1 = get_player_a1();
        if (a1 < 0x10000)
        {
            logger.info("Capture: player not found");
            return;
        }

        __try
        {
            const auto entry_desc = *reinterpret_cast<uintptr_t *>(a1 + auth_table::CONTAINER_PTR_OFFSET);
            if (entry_desc < 0x10000)
            {
                logger.warning("Capture: no entry table");
                return;
            }
            const auto entry_array =
                *reinterpret_cast<uintptr_t *>(entry_desc + auth_table::CONTAINER_ARRAY_BASE_OFFSET);
            const auto entryCount = *reinterpret_cast<uint32_t *>(entry_desc + auth_table::CONTAINER_COUNT_OFFSET);

            logger.info("=== CAPTURE: {} equipment slots ===", entryCount);

            namespace pws = Transmog::prefab_wrapper_swap;

            // Capture means "snapshot what the user wears right now". Any session-only pws prefab pick must be
            // cleared, so the captured carrier itemIds become the visible state. Otherwise the cyan prefab label hides
            // the captured gear behind a stale prefab pick. Disabled slots are skipped entirely: the dispatcher does
            // not service them, and captured state written into their mapping only bloats in-memory rows that the next
            // Save drops on disk anyway, per slot_metadata.hpp.
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                if (!Transmog::slot_enabled(i))
                    continue;
                const auto tslot = static_cast<TransmogSlot>(i);
                const int cur_src = pws::selection_src_index(tslot);
                pws::set_selection(tslot, cur_src, -1, "capture");
            }

            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                if (!Transmog::slot_enabled(i))
                    continue;
                slot_mappings()[i].active = true;
                slot_mappings()[i].target_item_id = 0;
            }

            int captured = 0;
            for (uint32_t e = 0; e < entryCount && entry_array > 0x10000; ++e)
            {
                const auto base = entry_array + e * auth_table::ENTRY_STRIDE;
                const auto game_slot = *reinterpret_cast<int16_t *>(base + auth_table::ENTRY_SLOT_TAG_OFFSET);
                const auto item_id = *reinterpret_cast<uint16_t *>(base + auth_table::ENTRY_ITEM_ID_OFFSET);

                logger.info("  Slot {:>2} ({:<12}) = item {:#06x}", game_slot, game_slot_name(game_slot), item_id);

                const auto tm_slot = slot_from_game_slot(game_slot);
                if (tm_slot.has_value() && item_id != 0 && item_id != 0xFFFF)
                {
                    const auto idx = static_cast<std::size_t>(*tm_slot);
                    if (!Transmog::slot_enabled(idx))
                    {
                        logger.info("    -> Skipping {} (slot disabled)", slot_name(*tm_slot));
                        continue;
                    }
                    slot_mappings()[idx].target_item_id = item_id;
                    ++captured;
                    logger.info("    -> Captured as {} target", slot_name(*tm_slot));
                }
            }

            // Live dye snapshot delegated to a separate function so its std::string mutations do not conflict with
            // this function's __try frame (MSVC C2712 forbids C++ object unwinding inside __try).
            capture_live_dye_into_active_preset(entry_array, entryCount);

            logger.info("=== CAPTURE DONE: {} equipped, {} total slots ===", captured, SLOT_COUNT);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("Capture: access fault");
        }
    }

    void capture_real_equipment()
    {
        auto &logger = DMK::log();
        const auto a1 = get_player_a1();
        if (a1 < 0x10000)
        {
            logger.info("capture_real_equipment: player not found");
            return;
        }

        // Capture-style snapshot: skip disabled slots to keep in-memory rows out for slots the dispatcher does not
        // service. pws picks are intentionally NOT preserved. This function is a "what does the user wear right now"
        // snapshot, and it mirrors capture_outfit. Any session-only prefab pick must surrender to the captured item_id,
        // so the visible state matches.
        namespace pws = Transmog::prefab_wrapper_swap;
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (!Transmog::slot_enabled(i))
                continue;
            const auto tslot = static_cast<TransmogSlot>(i);
            const int cur_src = pws::selection_src_index(tslot);
            pws::set_selection(tslot, cur_src, -1, "capture");
        }

        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (!Transmog::slot_enabled(i))
                continue;
            slot_mappings()[i].active = true;
            slot_mappings()[i].target_item_id = 0;
        }

        __try
        {
            const auto entry_desc = *reinterpret_cast<uintptr_t *>(a1 + auth_table::CONTAINER_PTR_OFFSET);
            if (entry_desc < 0x10000)
                return;
            const auto entry_array =
                *reinterpret_cast<uintptr_t *>(entry_desc + auth_table::CONTAINER_ARRAY_BASE_OFFSET);
            const auto entryCount = *reinterpret_cast<uint32_t *>(entry_desc + auth_table::CONTAINER_COUNT_OFFSET);

            for (uint32_t e = 0; e < entryCount && entry_array > 0x10000; ++e)
            {
                const auto base = entry_array + e * auth_table::ENTRY_STRIDE;
                const auto game_slot = *reinterpret_cast<int16_t *>(base + auth_table::ENTRY_SLOT_TAG_OFFSET);
                const auto item_id = *reinterpret_cast<uint16_t *>(base + auth_table::ENTRY_ITEM_ID_OFFSET);

                const auto tm_slot = slot_from_game_slot(game_slot);
                if (tm_slot.has_value() && item_id != 0 && item_id != 0xFFFF)
                {
                    const auto idx = static_cast<std::size_t>(*tm_slot);
                    if (!Transmog::slot_enabled(idx))
                        continue;
                    slot_mappings()[idx].target_item_id = item_id;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("capture_real_equipment: access fault");
        }
    }

    bool sync_live_dye_for_slot(std::size_t slot_idx) noexcept
    {
        auto &logger = DMK::log();
        if (slot_idx >= SLOT_COUNT)
            return false;
        if (!Transmog::slot_enabled(slot_idx))
        {
            logger.info("[dye-sync] slot {} disabled in mod config - skipped", slot_idx);
            return false;
        }

        auto *active_preset = PresetManager::instance().active_preset_mut();
        if (active_preset == nullptr)
            return false;
        if (slot_idx >= active_preset->slots.size())
            return false;

        const auto tslot = static_cast<TransmogSlot>(slot_idx);
        const auto game_tag = game_slot_from_transmog(tslot);

        const auto a1 = get_player_a1();
        if (a1 < 0x10000)
        {
            logger.info("[dye-sync] player not found - skipped");
            return false;
        }

        // Walk the auth-table container for the entry whose slot tag matches. Every read is guarded on its own, so a
        // torn table names the faulting address in the log instead of collapsing into the same bare miss an absent
        // slot produces.
        const auto field = [](std::uintptr_t base, std::ptrdiff_t disp) noexcept
        { return DMK::Address{base + static_cast<std::uintptr_t>(disp)}; };

        const auto entry_desc =
            DMK::memory::read<std::uintptr_t>(field(static_cast<std::uintptr_t>(a1), auth_table::CONTAINER_PTR_OFFSET));
        if (!entry_desc.has_value() || !DMK::memory::is_plausible_ptr(DMK::Address{*entry_desc}))
        {
            logger.info("[dye-sync] no auth-table container for slot {} - skipped", slot_name(tslot));
            return false;
        }

        const auto entry_array =
            DMK::memory::read<std::uintptr_t>(field(*entry_desc, auth_table::CONTAINER_ARRAY_BASE_OFFSET));
        const auto entryCount =
            DMK::memory::read<std::uint32_t>(field(*entry_desc, auth_table::CONTAINER_COUNT_OFFSET));
        if (!entry_array.has_value() || !entryCount.has_value() ||
            !DMK::memory::is_plausible_ptr(DMK::Address{*entry_array}))
        {
            logger.info("[dye-sync] auth-table container unreadable for slot {} - skipped", slot_name(tslot));
            return false;
        }

        std::uintptr_t entry_base = 0;
        for (std::uint32_t e = 0; e < *entryCount; ++e)
        {
            const auto base = *entry_array + static_cast<std::uintptr_t>(e) * auth_table::ENTRY_STRIDE;
            const auto tag = DMK::memory::read<std::int16_t>(field(base, auth_table::ENTRY_SLOT_TAG_OFFSET));
            if (!tag.has_value())
            {
                logger.warning("[dye-sync] auth-table entry {} unreadable at {}", e, DMK::format::format_address(base));
                break;
            }
            if (*tag == game_tag)
            {
                entry_base = base;
                break;
            }
        }

        if (entry_base == 0)
        {
            logger.info(
                "[dye-sync] no auth-table entry for slot {} (gameTag={:#x}) - skipped",
                slot_name(tslot),
                game_tag
            );
            return false;
        }

        dye_record_inject::ChannelState live[dye_record_inject::DYE_CHANNEL_COUNT];
        const auto dye_filled = dye_record_inject::read_entry_dye_records(entry_base, live);
        if (dye_filled == 0)
        {
            logger.info("[dye-sync] slot {} has no live dye records - preset slot left untouched", slot_name(tslot));
            return false;
        }

        dye_record_inject::log_dye_snapshot("sync", slot_name(tslot), live);

        // Wipe before writing so channels not present in `live` do not linger from a prior picker session (matches the
        // capture_outfit per-slot pattern).
        auto &slot_preset = active_preset->slots[slot_idx];
        for (auto &ch : slot_preset.dye)
            ch = ChannelDye{};

        const bool any = apply_live_dye_to_preset_slot(slot_preset, live) > 0;

        if (any)
        {
            dye_dirty().store(true, std::memory_order_release);
            logger.info("[dye-sync] slot {} captured {} channel(s) from live engine", slot_name(tslot), dye_filled);
        }
        return any;
    }

    // Init / Shutdown

    DMK::Result<void> init(DMK::Session &session, const WheelHostTable *wheel_host)
    {
        auto &logger = DMK::log();

        // Required asset gate
        //
        // The display-names TSV ships with the mod and is required for the catalog UI and name-keyed preset
        // resolution. A missing file means the user installed the mod incorrectly. Surface a hard, visible error
        // (modal popup) and bail out of init, so a user who never opens the log cannot ignore the failure.
        {
            // The wide directory keeps full Unicode fidelity and operator/ inserts the separator, so the
            // path that gets opened never passes through a narrow encoding. get_runtime_directory() falls
            // back to the working directory and finally to a relative anchor, so it is never empty and the
            // probe alone decides whether the asset is present.
            const std::filesystem::path tsv_path =
                std::filesystem::path{DMK::filesystem::get_runtime_directory()} / DISPLAY_NAMES_FILE;
            std::ifstream probe(tsv_path);
            if (!probe.is_open())
            {
                std::string body = "Required asset '" + std::string{DISPLAY_NAMES_FILE} +
                                   "' was not found.\n\nExpected location:\n  " + to_utf8(tsv_path) +
                                   "\n\nThe TSV must sit next to the mod DLL (same folder, "
                                   "wherever you installed it). Reinstall the mod and "
                                   "verify all files are present.\n\nThe mod will not function.";
                logger.error("{}", body);
                ::MessageBoxA(
                    nullptr,
                    body.c_str(),
                    "CrimsonDesertLiveTransmog - missing asset",
                    MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SYSTEMMODAL
                );
                return std::unexpected(DMK::Error{DMK::ErrorCode::FileOpenFailed, "Transmog::init"});
            }
        }

        // Apply config before the resolver and hook-install steps so the INI LogLevel takes effect for any TRACE/DEBUG
        // emissions that follow. Setters dispatched by the INI load touch only atomics, preset/state structures and
        // input bindings. None of them depend on resolved addresses, which are populated below.
        load_config(session);

        if (!DMK::memory::init_cache())
            logger.warning("Memory cache init failed - pointer reads may be slower");

        // Resolve AOB addresses
        //
        // These targets are independent: each scans a static candidate table and none reads another's resolved
        // address. They all live in the host EXE, so ONE parallel pass over the whole anchor registry resolves every
        // target the mod will ever need - these and the ones each feature module reads later - and the wall-clock
        // collapses to the slowest single scan instead of the sum. The targets are independent (no resolution reads
        // another's result), so the order below is presentation only. Every per-target validation and side-effect
        // block reads its address back through anchor_address(), which returns 0 for a ladder that missed or a value
        // its validator rejected.

        auto &addrs = resolved_addrs();

        resolve_all_anchors();

        // SlotPopulator: the KEY function for transmog.
        addrs.slot_populator = anchor_address(AnchorId::SlotPopulator);

        // PartSlotRefresh: rebuilds ONE slot's visual. SlotPopulator calls it with the slot derived from the ITEM,
        // which is the same value for both halves of a paired slot, so an apply to the second half rebuilds the
        // first. Calling it directly with the intended slot is what reaches the other half.
        {
            const auto refresh_addr = anchor_address(AnchorId::PartSlotRefresh);
            const auto tag_to_handle_addr = anchor_address(AnchorId::SlotTagToHandle);
            if (tag_to_handle_addr)
            {
                slot_tag_to_handle_fn() = reinterpret_cast<SlotTagToHandleFn>(tag_to_handle_addr);
                logger.info("SlotTagToHandle resolved at {:#x}", tag_to_handle_addr);
            }
            else
            {
                logger.warning("SlotTagToHandle AOB scan failed - paired slots cannot be refreshed");
            }
            if (refresh_addr)
            {
                part_slot_refresh_fn() = reinterpret_cast<PartSlotRefreshFn>(refresh_addr);
                logger.info("PartSlotRefresh resolved at {:#x}", refresh_addr);
            }
            else
            {
                logger.warning(
                    "PartSlotRefresh AOB scan failed - the second half of a paired slot "
                    "(Ring2/Earring2) will not refresh"
                );
            }
        }

        if (!addrs.slot_populator)
            logger.warning("SlotPopulator AOB scan failed - transmog will not work");

        // MapLookup: IndexedStringA::lookup. Not hooked - RIP anchor for scan_indexed_string_table(). Must be resolved
        // before part_show_suppress::init_slot_hashes.
        addrs.map_lookup = anchor_address(AnchorId::MapLookup);

        if (addrs.map_lookup)
        {
            // Deferred slot-hash resolution. A synchronous scan here observes a small or empty IndexedStringA table on
            // cold-launch, because LT loads before the game finishes wiring main-menu state. That leaves
            // part_show_suppress inert for the whole session. The deferred worker polls until world-ready, then commits
            // after every expected slot hash is present. See transmog_worker.hpp for the contract.
            launch_deferred_slot_hash_scan();
            logger.info("[dispatch] slot-hash resolution scheduled (deferred until world-ready)");
        }
        else
        {
            logger.warning(
                "MapLookup AOB scan failed - cannot resolve CD_* slot hashes, "
                "PartShowSuppress will be inert this session"
            );
        }

        // SubTranslator: anchor for the item-name catalog scan, and the item -> slot resolver LT calls to ask whether
        // a carrier can be placed at all. One function, both roles - see its cascade doc in aob_resolver.hpp.
        addrs.sub_translator = anchor_address(AnchorId::SubTranslator);
        if (addrs.sub_translator)
        {
            // Wire the resolver BEFORE the catalog build. The build can take the deferred path and hand off to the
            // worker, and no downstream caller must care whether the pointer landed first.
            item_to_slot_resolve_fn() = reinterpret_cast<ItemToSlotResolveFn>(addrs.sub_translator);

            using BR = ItemNameTable::BuildResult;
            const auto result = ItemNameTable::instance().build(addrs.sub_translator);
            if (result == BR::Ok)
            {
                logger.info("[nametable] built synchronously at init ({} entries)", ItemNameTable::instance().size());
                // Load display names BEFORE dump_catalog_tsv, so the sorted cache that the dump builds lazily already
                // contains display names. A second rebuild stalls the overlay render thread.
                {
                    ItemNameTable::instance().load_display_names(
                        std::filesystem::path{DMK::filesystem::get_runtime_directory()} / DISPLAY_NAMES_FILE
                    );
                }
                if (flag_dump_item_catalog().load(std::memory_order_relaxed))
                    ItemNameTable::instance().dump_catalog_tsv();
                if (flag_dump_item_prefabs().load(std::memory_order_relaxed))
                {
                    // The targeted phantom-recovery sweep can take minutes on a cold registry, so it runs on its own
                    // worker rather than blocking the rest of transmog init (hooks, color-override). Nothing
                    // downstream depends on the dump.
                    launch_itemmesh_dump();
                }
            }
            else if (result == BR::Deferred)
            {
                logger.info("[nametable] iteminfo global not initialized yet - starting background scan thread");
                launch_deferred_nametable_scan();
            }
            else // Fatal
            {
                logger.warning("[nametable] address chain resolution failed - item-name table disabled this session");
            }

            addrs.indexed_string_lookup = ItemNameTable::instance().indexed_string_lookup_addr();
            if (addrs.indexed_string_lookup)
            {
                logger.info("IndexedStringLookup cached at 0x{:X} (via chain walk)", addrs.indexed_string_lookup);
            }
        }
        else
        {
            logger.warning(
                "SubTranslator AOB scan failed - cannot build the item-name table (presets fall back to "
                "raw itemIds) and carrier equip-eligibility checks are unavailable"
            );
        }

        // SafeTearDown: scene-graph tear-down.
        addrs.safe_tear_down = anchor_address(AnchorId::SafeTearDown);
        if (!addrs.safe_tear_down)
        {
            logger.warning("SafeTearDown AOB scan failed - real_part_tear_down will be disabled this session");
        }

        // InitSwapEntry: zero-init helper for the 0x80-byte swap entry passed to SlotPopulator.
        {
            // The registry's code_site validator already applied the prologue-plausibility screen, so a site
            // that failed it arrives here as 0.
            const auto ise_addr = anchor_address(AnchorId::InitSwapEntry);

            if (ise_addr)
            {
                init_swap_entry_fn() = reinterpret_cast<InitSwapEntryFn>(ise_addr);
                logger.info("InitSwapEntry at 0x{:X}", ise_addr);
            }
            else
            {
                logger.warning("InitSwapEntry AOB scan failed - transmog apply will be disabled this session");
            }
        }

        // Load presets

        {
            const std::filesystem::path presets_path =
                std::filesystem::path{DMK::filesystem::get_runtime_directory()} / PRESETS_FILE;

            auto &pm = PresetManager::instance();
            pm.load(presets_path);
            pm.apply_to_state();
        }

        // Install hooks

        // SlotPopulator: resolved for direct call (not hooked).
        if (addrs.slot_populator)
        {
            if (DMK::scan::is_likely_function_prologue(DMK::Address{addrs.slot_populator}))
            {
                slot_populator_fn() = reinterpret_cast<SlotPopulatorFn>(addrs.slot_populator);
                logger.info("SlotPopulator resolved at 0x{:X}", addrs.slot_populator);
            }
            else
            {
                logger.warning(
                    "SlotPopulator resolved to 0x{:X} but prologue byte "
                    "looks wrong - rejecting, transmog apply disabled",
                    addrs.slot_populator
                );
                addrs.slot_populator = 0;
            }
        }

        // Deliberately NO BatchEquip or VisualEquipChange hook. Either one only notices a real equip change and
        // schedules an apply that re-dresses the slot afterwards, which is what makes the real item visible in the
        // meantime.
        //
        // socket_mesh_override is the whole equip-change mechanism instead. It overrides the mesh and its dye as the
        // part is BUILT, so the slot is already correct before anything is drawn. That also covers the paths an
        // equip-event hook never sees: an item-to-item replace, and several further callers that reach the same
        // builder.
        //
        // The player component caches lazily through resolve_player_component, and the load-detect thread stores it
        // each poll. The load-detect thread's body-change branch resets the per-character ledger on an actor change.
        //
        // If a slot ever keeps a stale look until the UI is touched, re-examine this. The override only fires when
        // the engine BUILDS a part, so a visual change with no rebuild has nothing to drive it.

        // PartAddShow inline hook - transition-flash polish.
        //
        // Auto-skip if CrimsonDesertEquipHide is loaded in-process. EH installs its own inline hook on the exact same
        // function for its gliding-fix. Two inline hooks on one address chain non-deterministically across game
        // launches, because DLL load order is not fixed, and one side can end up silently bypassed. LT's hook is
        // cosmetic polish and EH's hook is user-visible functionality, so when both are present, yield to EH.
        //
        // A managed-hook-versus-managed-hook collision check covers only the case where both sides hook through the
        // same hook manager. The substring module-name match here covers the orthogonal case where EH is loaded but
        // did not install its hook yet (load-order race during the worker thread's init pass), or where EH hooks
        // through a non-DMK route. A yield on module presence avoids the race entirely. The substring match covers
        // both the dev two-DLL ("..._Logic.dll") and the release single-ASI (".asi") layouts in one call.
        // EquipHide ships as a single ASI in release and, in its dev configuration, as a resident loader ASI that
        // stages the logic DLL. Both shapes load the same .asi module name, so one exact-basename query covers them.
        const bool eh_present = DMK::memory::is_module_loaded(EQUIP_HIDE_MODULE);
        if (eh_present)
        {
            logger.info(
                "[dispatch] PartAddShow hook skipped - CrimsonDesertEquipHide detected; yielding to "
                "its gliding-fix hook to avoid dual-install ordering issues."
            );
        }
        else
        {
            // The registry's code_site validator already applied the prologue-plausibility screen, so a site that
            // failed it arrives here as 0.
            const auto pas_addr = anchor_address(AnchorId::PartAddShow);

            if (pas_addr)
            {
                auto pas_hook = DMK::hook::inline_at(
                    DMK::hook::InlineRequest{
                        .name = "PartAddShow",
                        .target = DMK::Address{pas_addr},
                    },
                    &part_show_suppress::on_part_add_show
                );
                bool pas_ok = false;
                if (!pas_hook)
                {
                    logger.warning("PartAddShow hook creation failed: {}", pas_hook.error().message());
                }
                else
                {
                    // Publish the trampoline BEFORE enable() arms the patch, so no game thread can enter the detour
                    // while its original pointer is still null.
                    part_show_suppress::set_part_add_show_trampoline(
                        pas_hook->original<part_show_suppress::PartAddShowFn>()
                    );
                    if (auto armed = pas_hook->enable(); !armed)
                    {
                        logger.warning("PartAddShow hook could not be armed: {}", armed.error().message());
                        part_show_suppress::set_part_add_show_trampoline(nullptr);
                    }
                    else
                    {
                        s_hooks.push(std::move(*pas_hook));
                        pas_ok = true;
                    }
                }
                if (!pas_ok)
                {
                    logger.warning("PartAddShow hook failed - transition flash suppression disabled");
                }
            }
            else
            {
                logger.warning("[dispatch] PartAddShow AOB scan failed - transition flash suppression disabled");
            }
        }

        // Real-part scene-graph tear-down.
        if (!real_part_tear_down::resolve_helpers())
        {
            logger.warning("[dispatch] tear_down: helper resolution failed - feature disabled");
        }

        // Input

        // Resolve WorldSystem pointer for LT-local chain walks (per-character presets, load-detect, apply-side a1
        // fallbacks). Independent of CDCore::controlled_char, which uses its own static-chain anchor and does not need
        // a published WorldSystem holder.
        {
            const auto ws_addr = anchor_address(AnchorId::WorldSystem);
            if (ws_addr)
            {
                world_system_ptr().store(ws_addr, std::memory_order_release);
                logger.info("WorldSystem pointer at 0x{:X}", ws_addr);
            }
            else
            {
                logger.warning("WorldSystem AOB failed - load-time transmog and per-character presets disabled");
            }
        }

        // Install BEFORE anything can drive an equip or a tear-down. The guard makes the engine's claim-vector walks
        // tolerate the null-owner window its own non-atomic erase opens. Until it is in place, any erase that
        // overlaps a walk on a job thread can fault. See claim_walk_guard.hpp.
        // install() logs its own failure and the mod still runs without the guard, so the result is discarded here.
        (void)claim_walk_guard::install(s_hooks);

        // init() logs its own failure. Without the swap the mod applies carriers but never redirects their meshes.
        (void)prefab_wrapper_swap::init(s_hooks);

        // Override the mesh a socket is about to wear, so the engine never builds a real item for a slot LT dresses.
        // Installed after pws because it reads pws's per-slot target. See socket_mesh_override.hpp.
        // install() logs its own failure. A failure here leaves every transmogged slot rendering its real item, so
        // the log line is the only signal and the mod stays up.
        (void)socket_mesh_override::install(s_hooks);

        // Helm-audio filter. It intervenes at the passive-skill REGISTRATION boundary, BEFORE the muffle tag enters
        // the character's skill registry, so no downstream Wwise / RTPC / Switch path ever observes it. The combined
        // gates (audio-classifier call layout, RTTI chain walk to `pa::GameAudioEffectBuffData`, protagonist host)
        // leave the other passive-skill effects of the same item intact and keep NPC voices muffled as in vanilla. See
        // helm_audio_filter.hpp for the full data-flow chain and the bypass-safety analysis.
        //
        // Gated by `[Experimental] UnmuffleHelmVoice`. The flag is read once here at startup. A runtime toggle is not
        // supported, because a tear-down of the inline detour races the engine equip pipeline, which calls it from
        // arbitrary threads. The hook leaves a single virtual call un-invoked on SUPPRESS. The user opt-out is the
        // safety lever for that bypass.
        if (flag_helm_audio_unmuffle().load(std::memory_order_relaxed))
        {
            helm_audio_filter::init(s_hooks);
        }
        else
        {
            DMK::log().info(
                "[helm-audio] disabled; set "
                "`[Experimental] UnmuffleHelmVoice = true` to remove the stock plate/heavy-helm voice muffle."
            );
        }

        // Per-slot dye-record injector. Hooks the engine's dye-copier primitive and appends fabricated ARMOR_MOD
        // records so fake transmog items render with user-chosen colors regardless of the underlying real item.
        dye_record_inject::init(s_hooks);

        // color_override is a tri-hook subsystem (host-scope owner vfuncs, setter property substitute, publisher
        // per-matInst capture). The `[Experimental] color_override` INI key gates it, so the hooks do not install on
        // the default configuration. The picker UI keys off the same flag.
        if (flag_color_override().load(std::memory_order_acquire))
        {
            color_override::host_scope::init(s_hooks);
            color_override::setter_substitute::init(s_hooks);
            Transmog::color_override::init(s_hooks);
        }
        else
        {
            DMK::log().info("[color-override] disabled by [Experimental] ColorOverride=false; subsystem skipped");
        }

        // Crimson Desert has TWO independent dye layers:
        //   1. Bench/menu UI dyeability - gated by the partprefabdyeslotinfo.pabgb registry. LT does not modify it at
        //      runtime. Static PAZ overlays handle this externally.
        //   2. Render-time dye apply - the engine reads dye records from a publish vector at dst+120 during slotpop.
        //      The dye_record_inject inline detour on DyeCopier (see dye_copier(), init above) injects
        //      user-chosen records here.

        start_load_detect_thread();
        ensure_apply_worker_started();

        // load_config() registers the hotkey bindings so the INI load picks up their combo keys. Bring the poll
        // engine live now. The bindings start to fire on the next tick. The wheel backend is the dev loader's resident
        // host when one was supplied, so a user-bound mouse-wheel combo books its permanent module keepalive against
        // that module instead of this one and the logic DLL stays unmappable. Without a host (the production ASI, and
        // a dev loader whose host failed to start) the local message hook is the correct backend and takes that
        // keepalive here.
        DMK::input::Input::Settings input_settings{};
        if (wheel_host != nullptr)
        {
            input_settings.wheel_backend = DMK::input::Input::WheelBackend::ExternalHost;
            input_settings.wheel_host = wheel_host;
            input_settings.wheel_host_required = false;
        }
        if (auto started = session.input().start(input_settings); !started)
        {
            logger.warning("Input engine did not start: {} - hotkeys are inactive", started.error().message());
        }

        logger.info(
            "Transmog initialization complete - SlotPopulator {}",
            slot_populator_fn() ? "READY" : "UNAVAILABLE"
        );

        // One-shot DMK health snapshot for per-launch diagnostics: hook population plus any intentional loader-lock
        // leak or detach event.
        const auto health = DMK::diagnostics::collect({}, anchor_report());
        logger.info(
            "DMK health: hooks total={} active={} disabled={}, intentional-leaks={}",
            health.hooks_total,
            health.hooks_active,
            health.hooks_disabled,
            health.total_intentional_leaks
        );

        return {};
    }

    bool shutdown()
    {
        DMK::log().info("{} shutting down...", MOD_NAME);

        // Module pins, by reason, sampled HERE rather than at the end of init(). The input engine mounts its
        // wheel route and installs its XInput interception from the poll thread's cycle loop, which only starts
        // after session.input().start() returns, so an init-time sample reads a state where neither step ran yet
        // and reports zeros that mean nothing. This runs before any teardown, so the hooks and pins are both still
        // in place.
        //
        // XInputKeepalive is taken before XInput hook creation: nonzero means a binding asked to consume and the
        // interception pair went in. That set is retained and inert after teardown, so it keeps this image mapped
        // and costs one generation against the dev loader's reload budget.
        //
        // MessageHookKeepalive is the wheel-capture keepalive. In the dev build it reads ZERO. The resident loader
        // owns wheel capture, so the pin lands on the loader and this generation can still unmap. Nonzero means the
        // external host was refused and the engine fell back to a local message hook.
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

        // Disable the INI watcher up front so an in-flight save event cannot fire setters during the state tear-down.
        DMK::config::disable_auto_reload();

        shutdown_requested().store(true, std::memory_order_release);

        // Drain workers before the hooks they call into come down. Every spawned worker calls raw game functions
        // under SEH (apply_all_transmog -> SlotPopulator, debounce worker -> real_part_tear_down -> safe_tear_down).
        // Join them first to guarantee that no worker sits mid-call inside a trampoline when the trampoline pages are
        // unmapped.
        stop_load_detect_thread();
        stop_apply_worker();
        join_deferred_nametable_scan();
        join_deferred_slot_hash_scan();
        // The token-discovery re-scan walks the host image from its own worker and touches this module's statics, so
        // it has to be gone before the dev loader can unmap the generation.
        color_override::token_slot_discovery::stop_and_join_rescan();
        // The TSV dump waits for a swap catalog that never appears at the main menu, so it must be stopped rather
        // than waited out.
        join_itemmesh_dump();

        prefab_wrapper_swap::shutdown();

        // Newest-first teardown of every hook this mod owns (PartAddShow, the claim-walk guard, the socket-mesh
        // override and the rest). Each detour body snapshots its trampoline pointer at entry and bails to a benign
        // default if the snapshot is null, which defends the brief drain window between restore and DLL unmap.
        //
        // A hook that cannot prove it restored its target pins its backend, and that pin books one leak against
        // LeakSubsystem::HookManager, so the delta across the clear IS the unmap authorization the dev loader needs.
        // Comparing a delta rather than an absolute is required, because the counter also carries caller-requested
        // leaks from elsewhere.
        const auto pins_before = DMK::diagnostics::intentional_leak_count(DMK::diagnostics::LeakSubsystem::HookManager);
        s_hooks.clear();
        const bool restored =
            DMK::diagnostics::intentional_leak_count(DMK::diagnostics::LeakSubsystem::HookManager) == pins_before;
        if (!restored)
        {
            DMK::log().error("{} shutdown: a hooked prologue could not be restored and stays pinned", MOD_NAME);
        }

        DMK::log().info("{} shutdown complete", MOD_NAME);
        return restored;
    }

} // namespace Transmog
