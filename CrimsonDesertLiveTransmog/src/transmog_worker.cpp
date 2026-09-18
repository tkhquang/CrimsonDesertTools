#include "transmog_worker.hpp"
#include "indexed_string_table.hpp"
#include "part_show_suppress.hpp"
#include "prefab_wrapper_swap.hpp"
#include "constants.hpp"
#include "game_thread.hpp"
#include "item_name_table.hpp"
#include "itemmesh_dumper.hpp"
#include "preset_manager.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog.hpp"
#include "transmog_apply.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>

namespace Transmog
{
    // Sleeps @p ms in 100 ms slices and returns early on a stop request or on shutdown. Every worker body in this TU
    // waits through it, so one stop signal ends a wait within 100 ms wherever it lands.
    static void sleep_interruptible(std::stop_token stop, int ms)
    {
        for (int i = 0; i < ms / 100; ++i)
        {
            if (stop.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                return;
            Sleep(100);
        }
    }

    // Deferred item-name catalog scan
    //
    // The game populates the iteminfo global some time after the LT init runs - exactly when that lands
    // depends on world load order, so init() cannot build the catalog reliably. Mirror EquipHide's pattern: a
    // single background thread that sleeps an initial grace period, then retries `ItemNameTable::build` until
    // `BuildResult::Ok` or the attempt budget is exhausted.
    //
    // Once the build lands, the thread calls PresetManager to re-resolve every stored item_name against the freshly
    // populated catalog and re-applies the active preset so the visual snaps into place.
    static std::mutex s_nametable_thread_mtx;
    static std::optional<DMK::StoppableWorker> s_nametable_scan_worker;
    static std::atomic<bool> s_nametable_scan_launched{false};

    static constexpr int NAMETABLE_INITIAL_DELAY_MS = 8000;
    static constexpr int NAMETABLE_RETRY_MS = 2000;

    static void deferred_nametable_scan_fn(std::stop_token stop) noexcept
    {
        auto &logger = DMK::log();
        const auto sub_translator = resolved_addrs().sub_translator;
        if (!sub_translator)
            return;

        using BR = ItemNameTable::BuildResult;

        // Initial grace period - lets the game finish its iteminfo container load before the poll loop starts.
        sleep_interruptible(stop, NAMETABLE_INITIAL_DELAY_MS);
        if (stop.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
            return;

        int attempt = 0;
        for (;;)
        {
            if (stop.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                return;

            const auto result = ItemNameTable::instance().build(sub_translator);
            if (result == BR::Ok)
            {
                const auto size = ItemNameTable::instance().size();
                logger.info("[nametable] deferred scan succeeded on attempt {} ({} entries)", attempt + 1, size);
                // Load display names before dump so the sorted cache built by the dump already contains them.
                {
                    ItemNameTable::instance().load_display_names(
                        std::filesystem::path{DMK::filesystem::get_runtime_directory()} / DISPLAY_NAMES_FILE
                    );
                }
                if (flag_dump_item_catalog().load(std::memory_order_relaxed))
                    ItemNameTable::instance().dump_catalog_tsv();
                if (flag_dump_item_prefabs().load(std::memory_order_relaxed))
                {
                    // Targeted phantom-recovery sweep can take ~minutes, so it runs on its own worker and
                    // reresolve_all_names + manual_apply happen immediately without waiting on the TSV write.
                    launch_itemmesh_dump();
                }

                // Re-resolve all loaded preset slots against the now-populated catalog and re-apply the active preset
                // so the live slot_mappings picks up any drift corrections.
                auto &pm = PresetManager::instance();
                pm.reresolve_all_names();
                pm.apply_to_state();

                // Push the corrected state through the apply pipeline so the visible transmog reflects whatever the
                // deferred resolution repaired.
                manual_apply();
                return;
            }
            if (result == BR::Fatal)
            {
                logger.error(
                    "[nametable] deferred scan hit fatal chain error - item catalog unavailable, mod disabled"
                );
                flag_enabled().store(false, std::memory_order_release);
                return;
            }

            // Deferred: sleep and retry indefinitely until the game populates the catalog or shutdown is requested.
            ++attempt;
            if (attempt % 50 == 0)
                logger.debug("[nametable] still waiting after {} attempts", attempt);

            sleep_interruptible(stop, NAMETABLE_RETRY_MS);
            if (stop.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                return;
        }
    }

    void launch_deferred_nametable_scan() noexcept
    {
        if (s_nametable_scan_launched.exchange(true, std::memory_order_acq_rel))
            return;

        std::lock_guard<std::mutex> lk(s_nametable_thread_mtx);
        s_nametable_scan_worker.emplace("LtNametableScan", &deferred_nametable_scan_fn);
    }

    void join_deferred_nametable_scan()
    {
        std::lock_guard<std::mutex> lk(s_nametable_thread_mtx);
        // ~StoppableWorker requests stop and joins, so the reset IS the join.
        s_nametable_scan_worker.reset();
    }

    // Deferred part_show_suppress slot-hash scan
    //
    // Mirrors deferred_nametable_scan_fn but for the IndexedStringA entries part_show_suppress keys on. A synchronous
    // scan at LT init observes a small or empty table on cold-launch (LT loaded before main-menu wiring finishes),
    // leaving part_show_suppress inert for the entire session. The deferred worker gates on
    // Transmog::is_world_ready() and waits until every expected part_show_hash_key resolves, then commits once.
    static std::mutex s_slot_hash_thread_mtx;
    static std::optional<DMK::StoppableWorker> s_slot_hash_scan_worker;
    static std::atomic<bool> s_slot_hash_scan_launched{false};

    // Timings match NAMETABLE_INITIAL_DELAY_MS / NAMETABLE_RETRY_MS so both deferred workers behave the same way during
    // cold-launch.
    static constexpr int SLOT_HASH_INITIAL_DELAY_MS = 8000;
    static constexpr int SLOT_HASH_RETRY_MS = 2000;

    static std::size_t expected_slot_hash_count() noexcept
    {
        std::size_t expected = 0;
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            const char *key = SLOT_METADATA[i].part_show_hash_key;
            if (key && key[0] != '\0')
                ++expected;
        }
        return expected;
    }

    static void deferred_slot_hash_scan_fn(std::stop_token stop) noexcept
    {
        auto &logger = DMK::log();
        const auto map_lookup_addr = resolved_addrs().map_lookup;
        if (!map_lookup_addr)
            return; // MapLookup unresolved - already warned at init.

        const auto expected_count = expected_slot_hash_count();
        if (expected_count == 0)
            return; // Nothing to resolve (no part_show_hash_key rows).

        // Initial grace period before the first scan. It mirrors the nametable worker's initial delay, so the loop
        // does not waste polls while the engine is still in pre-main-menu state.
        sleep_interruptible(stop, SLOT_HASH_INITIAL_DELAY_MS);
        if (stop.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
            return;

        std::size_t prev_resolvable = 0;

        for (int attempt = 1;; ++attempt)
        {
            sleep_interruptible(stop, SLOT_HASH_RETRY_MS);
            if (stop.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                return;

            // World-ready gate. Without a live world the IndexedStringA table carries only a few engine-internal seed
            // entries and the CD_* part names are not yet registered.
            if (!Transmog::is_world_ready())
            {
                if (attempt % 50 == 0)
                    logger.debug("[dispatch] slot-hash deferred scan: waiting for world after {} attempts", attempt);
                continue;
            }

            auto name_to_hash = scan_indexed_string_table(map_lookup_addr);
            if (name_to_hash.empty())
            {
                if (attempt % 50 == 0)
                    logger.debug(
                        "[dispatch] slot-hash deferred scan: IndexedStringA empty after world-ready ({} attempts)",
                        attempt
                    );
                continue;
            }

            // probe how many target keys are present without mutating part_show_suppress yet. Keeps the publish
            // path single-shot so the hook sees one atomic transition from "inert" to "fully ready".
            std::size_t resolvable = 0;
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                const char *key = SLOT_METADATA[i].part_show_hash_key;
                if (!key || key[0] == '\0')
                    continue;
                if (name_to_hash.find(key) != name_to_hash.end())
                    ++resolvable;
            }

            if (resolvable == expected_count)
            {
                const auto resolved = part_show_suppress::init_slot_hashes(name_to_hash);
                logger.info(
                    "[dispatch] slot hashes resolved via deferred scan: {}/{} slots ({} attempts)",
                    resolved,
                    expected_count,
                    attempt
                );
                return;
            }

            if (resolvable > prev_resolvable)
            {
                logger.debug(
                    "[dispatch] slot-hash deferred scan: {}/{} resolvable (attempt {})",
                    resolvable,
                    expected_count,
                    attempt
                );
                prev_resolvable = resolvable;
                continue;
            }

            // Plateaued - sleep and retry indefinitely until every expected key registers or shutdown is requested.
            if (attempt % 50 == 0)
                logger.debug(
                    "[dispatch] slot-hash deferred scan: still waiting after {} attempts ({}/{} resolvable)",
                    attempt,
                    resolvable,
                    expected_count
                );
        }
    }

    void launch_deferred_slot_hash_scan() noexcept
    {
        if (s_slot_hash_scan_launched.exchange(true, std::memory_order_acq_rel))
            return;

        std::lock_guard<std::mutex> lk(s_slot_hash_thread_mtx);
        s_slot_hash_scan_worker.emplace("LtSlotHashScan", &deferred_slot_hash_scan_fn);
    }

    void join_deferred_slot_hash_scan()
    {
        std::lock_guard<std::mutex> lk(s_slot_hash_thread_mtx);
        // ~StoppableWorker requests stop and joins, so the reset IS the join.
        s_slot_hash_scan_worker.reset();
    }

    // Player component resolution
    //
    // WorldSystem -> ClientActorManager -> ClientUserActor chain
    // offsets. The same pa::ClientActorManager singleton is reached via the WorldSystem holder here and via the
    // published ClientActorManagerGlobal in CDCore's controlled_char.cpp. The layout offsets are owned by CDCore
    // (actor_chain_offsets, controlled_char.hpp) so a struct re-layout is a single edit there. These aliases keep the
    // local names the consumers below use.
    constexpr std::ptrdiff_t WS_TO_ACTOR_MANAGER = CDCore::actor_chain_offsets::WORLD_SYSTEM_TO_ACTOR_MANAGER;
    constexpr std::ptrdiff_t ACTOR_MANAGER_TO_USER = CDCore::actor_chain_offsets::ACTOR_MANAGER_TO_USER_ACTOR;
    constexpr std::ptrdiff_t USER_TO_CONTROLLED = CDCore::actor_chain_offsets::USER_ACTOR_TO_CONTROLLED;

    // Structural plausibility screen for an engine pointer. The apply entry points carry the engine's signed __int64
    // argument, and a negative value widens to a non-canonical address that the canonical upper bound rejects. The
    // cast back to std::uintptr_t is bit-preserving, so one overload serves both the signed and unsigned call sites.
    static bool plausible_engine_ptr(__int64 value) noexcept
    {
        return DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(value)});
    }

    // Guarded walk WorldSystem -> ActorManager -> UserActor.
    // Returns the validated pa::ClientUserActor pointer, or 0 on a null WorldSystem holder or any torn/faulting
    // intermediate. Single source of truth for resolve_player_component(), read_user_actor_ptr() and
    // read_controlled_actor_ptr(), which otherwise each repeat this exact three-link walk.
    static std::uintptr_t walk_ws_to_user_actor() noexcept
    {
        const auto ws_base = world_system_ptr().load(std::memory_order_acquire);
        if (!ws_base)
            return 0;

        // The walk screens every dereferenced link into the canonical user-mode window, so a torn link fails closed.
        // The trailing 0 makes the UserActor pointer the leaf: the last offset is added without a dereference.
        static constexpr std::ptrdiff_t chain[] = {0, WS_TO_ACTOR_MANAGER, ACTOR_MANAGER_TO_USER, 0};
        const auto user = DMK::memory::walk(DMK::Address{ws_base}, chain);
        return user ? user->raw() : 0;
    }

    // Actor-side hops from pa::ClientUserActor down to the equip-slot component that the apply pipeline treats as a1.
    //
    // These belong to the ACTOR, not to the component. Do not move them when the component layout shifts: the
    // component's own fields (container, slot cache) live in transmog_apply.cpp and change independently.
    //
    // ACTOR_TYPE_ENTRY_OFFSET is read only as a liveness probe, so a shift there fails CLOSED and stops every apply
    // with no log line. If applies stop firing and no anchor reports a miss, verify this offset first.
    constexpr std::ptrdiff_t ACTOR_TYPE_ENTRY_OFFSET = 0x88;
    constexpr std::ptrdiff_t ACTOR_TO_COMPONENT_HOLDER = 0x68;
    constexpr std::ptrdiff_t COMPONENT_HOLDER_TO_COMPONENT = 0x38;

    __int64 resolve_player_component() noexcept
    {
        const auto user = walk_ws_to_user_actor();
        if (!plausible_engine_ptr(static_cast<__int64>(user)))
            return 0;

        // user+0xD8 holds the currently-controlled character's ClientChildOnlyInGameActor. It coincides with user+0xD0
        // (the "primary" slot, always Kliff) when Kliff is the active character, and rotates to Damiane's or Oongka's
        // actor when one of them is controlled. A read of +0xD0 lands every companion apply on Kliff's actor, so the
        // apply pipeline always walks +0xD8.
        constexpr std::size_t chain_hops = 4;
        static constexpr std::ptrdiff_t chain[chain_hops] =
            {USER_TO_CONTROLLED, ACTOR_TO_COMPONENT_HOLDER, COMPONENT_HOLDER_TO_COMPONENT, 0};

        // trace[0] captures the controlled actor, which the liveness probe below needs.
        DMK::Address trace[chain_hops]{};
        const auto component = DMK::memory::walk(DMK::Address{user}, chain, trace);
        if (!component)
            return 0;

        // Pointer-validity check on the type_entry slot. The byte at type_entry+1 is role-based (controlled against
        // backgrounded) and is not stable per character, so it cannot gate the chain walk. A reject on it silently
        // drops companion applies. A readable type_entry pointer is sufficient structural evidence that the actor is
        // alive.
        const auto type_entry = DMK::memory::read<std::uintptr_t>(trace[0].offset(ACTOR_TYPE_ENTRY_OFFSET)).value_or(0);
        if (!plausible_engine_ptr(static_cast<__int64>(type_entry)))
            return 0;

        return static_cast<__int64>(component->raw());
    }

    // Debounced apply scheduling
    // Rapid inventory swaps fire VEC/BatchEquip hooks multiple times in quick succession. Without coalescing, each
    // trigger spawns its own apply and the visible effect is the player cycling head-to-toe while the auth table is
    // still mid-mutation (SEH faults in tear_down). The debounce collapses bursts into a single apply APPLY_DEBOUNCE_MS
    // after the last trigger.
    static std::atomic<std::uint64_t> s_apply_deadline_tick{0};
    static std::atomic<bool> s_apply_pending{false};

    // Tick of the previous apply REQUEST (not of the previous apply). Zero until the first request. This is what
    // decides whether an incoming request belongs to a burst - see schedule_transmog_ms and BURST_COALESCE_MS.
    static std::atomic<std::uint64_t> s_last_request_tick{0};

    // One-shot redirect: the editing character's 1-based idx that the next scheduled apply targets instead of the
    // controlled body. Set by overlay-UI entry points via set_targeted_apply_char_idx() and exchange-consumed by
    // run_debounced_apply. Engine-triggered hook paths never touch this atomic, so the controlled body remains the
    // default target for VEC / BatchEquip events even while the user is editing a non-controlled character.
    static std::atomic<std::uint32_t> s_targeted_apply_char_idx{0};

    // Set by run_debounced_apply: true if the last apply completed without SEH fault, false on exception. Used by
    // load-detect to stop retrying once a boot-time apply succeeds.
    static std::atomic<bool> s_last_apply_ok{false};

    // True while load_detect_thread_fn has a controlled-character change parked in its settle window but not yet
    // committed. The settle-window branch is the authoritative committer: it flips pm.active_character() and schedules
    // the apply once the candidate identity holds for CHAR_SWAP_SETTLE_MS.
    //
    // Without this gate, sync_active_char_to_live() flips inline the moment the controlled-char probe returns the
    // new identity, but the engine may not have rotated user+0xD8 to the new actor yet. A hook-driven apply that races
    // into run_debounced_apply during that window resolves a1 to the previous body and paints the new character's
    // preset onto it (e.g. save-load on
    // Kliff that auto-toggles to Oongka leaving Kliff wearing
    // Oongka's preset). sync_active_char_to_live consults this flag and returns false to defer. The caller re-arms the
    // debounce until the settle commits.
    static std::atomic<bool> s_char_swap_pending{false};

    // Multi-character auto-apply request, set by the load-detect thread when CDCore::world_generation() bumps (engine
    // has reallocated Kliff's CCOIA: cold-load or save-load). Consumed at the top of run_debounced_apply - the worker
    // walks CDCore::snapshot_body_cache(), then for each of the 1-3 player CCOIAs swaps PresetManager's active
    // character to that char, resolves its equip-slot via CDCore::equip_slot_for_ccoia(), and invokes
    // apply_all_transmog so every protagonist gets its saved preset on world entry without needing the user to cycle to
    // them.
    static std::atomic<bool> s_multi_char_apply_pending{false};

    static std::mutex s_apply_cv_mtx;
    static std::condition_variable s_apply_cv;
    static std::optional<DMK::StoppableWorker> s_apply_worker;
    static std::atomic<bool> s_apply_worker_started{false};

    // Apply ALWAYS targets the currently-controlled character. This helper re-reads the live character via the Core
    // resolver and, when PresetManager has a different controlled character cached, switches to the live one and
    // rebuilds slot_mappings from its preset. It holds no __try block, so std::string use is fine. Its caller is
    // run_debounced_apply, which cannot mix __try with C++ objects.
    //
    // The editing axis is left untouched here: if the user has the overlay dropdown pinned to a different character,
    // that pin persists across in-game character swaps and the cross-body apply remains in effect.
    //
    // Returns true when the apply may proceed, false when the caller must defer (re-arm the debounce). False is
    // returned only while load_detect_thread_fn has an unfired char-swap parked in its settle window: see
    // s_char_swap_pending.
    [[nodiscard]] static bool sync_active_char_to_live() noexcept
    {
        const std::string live = current_controlled_character_name();
        if (live.empty())
            return true;
        auto &pm = PresetManager::instance();
        if (live == pm.active_character())
            return true;

        // Defer when load_detect_thread_fn has a pending swap in flight. Convergence is bounded by the settle window
        // (CHAR_SWAP_SETTLE_MS). The caller re-arms via a 200ms schedule_transmog_ms() until then.
        if (s_char_swap_pending.load(std::memory_order_acquire))
            return false;

        pm.set_active_character(live);
        // Release a stale dropdown pin when the new controlled character is a third protagonist (not the previously
        // controlled, not the pinned). set_active_character only auto-clears the pin when the new controlled IS the
        // pinned char. In the third-character case (user pins Damiane while controlling Kliff, then swaps to Oongka
        // in-game) an engaged pin makes apply_to_state below populate slot_mappings from Damiane's preset, and the
        // next apply lands Damiane's outfit on Oongka. Treat the dropdown selection as transient
        // across real controlled-character swaps: in-game swap releases the pin so editing follows the new body.
        if (pm.editing_pinned() && pm.editing_character() != live)
        {
            pm.clear_editing_pin();
        }
        for (auto &m : slot_mappings())
        {
            m.active = false;
            m.target_item_id = 0;
        }
        pm.apply_to_state();
        last_applied_ids().fill(0);
        real_damaged().fill(false);
        last_applied_real_ids().fill(0);
        last_applied_carrier_ids().fill(0);
        return true;
    }

    // Per-character applied-CCOIA accessors. The backing state and definitions live with the tracking block further
    // down, immediately above rebind_preset_to_controlled. The forward declaration is here because apply_for_one_char
    // consults them to decide whether the target body was reallocated since its last successful apply.
    [[nodiscard]] static std::uintptr_t applied_ccoia_for_char(std::uint32_t idx) noexcept;
    static void set_applied_ccoia_for_char(std::uint32_t idx, std::uintptr_t ccoia) noexcept;

    // Multi-character auto-apply (world-generation triggered)
    //
    // Apply one character's preset against an explicit equip-slot a1, bypassing the controlled-char-only path that
    // run_debounced_apply normally takes. Mutates the same global axes (active_character, slot_mappings,
    // last_applied_*) that the single-char path does, so the caller must invoke it from the apply job only
    // (no concurrent writer). Logs failures at debug level since idle (non-controlled) protagonists have engine-zeroed
    // component fields that cause expected chain faults. Returns true if the apply ran (regardless of outcome), false
    // if the body was not yet ready and the caller must re-arm the multi-apply pending flag to retry later.
    [[nodiscard]] static bool apply_for_one_char(const std::string &name, std::uintptr_t ccoia) noexcept
    {
        auto &logger = DMK::log();
        if (name.empty() || ccoia == 0)
            return true; // nothing to do, the caller must not retry

        const auto equip_slot = CDCore::equip_slot_for_ccoia(ccoia);
        if (equip_slot == 0)
        {
            logger.debug(
                "[multi-apply] {} (ccoia=0x{:X}): equip-slot walk failed - skipping",
                name,
                static_cast<std::uint64_t>(ccoia)
            );
            return true; // CCOIA likely invalid; do not retry
        }

        // Gate on real_part_tear_down::is_actor_apply_ready. Idle / freshly-summoned bodies have partially-initialized
        // mesh containers: tear_down's SEH wrapper catches per-slot faults, but the post-apply pipeline downstream of
        // the first carrier write reads from the same container and throws on the unwired entries. Symptom:
        // our_written_count increases to 1, then apply_all_transmog raises out of the outer __try and leaves the body
        // half-applied. The load-detect thread's readiness gate uses this same predicate, so one policy covers both
        // entry points.
        if (!real_part_tear_down::is_actor_apply_ready(reinterpret_cast<void *>(equip_slot)))
        {
            logger.debug(
                "[multi-apply] {} a1=0x{:X}: body not yet ready (container chain incomplete) - will retry",
                name,
                static_cast<std::uint64_t>(equip_slot)
            );
            return false; // the caller must re-arm
        }

        // Swap PresetManager onto this character's axis. Mirrors the sync_active_char_to_live() body so slot_mappings
        // reflect the target preset before apply_all_transmog reads them.
        auto &pm = PresetManager::instance();
        pm.set_active_character(name);
        for (auto &m : slot_mappings())
        {
            m.active = false;
            m.target_item_id = 0;
        }
        pm.apply_to_state();

        // Hydrate the globals from this character's snapshot, but ONLY when the re-apply runs against the SAME body
        // the last apply targeted. The four applied-state globals describe the fakes currently installed on a body.
        // They are the truth source apply_all_transmog's no-change early-out and Phase A teardown consult.
        //
        // When this character's CCOIA pointer differs from the one the last successful apply used, the engine has
        // reallocated the body (off-screen stream-out + return, follower injury cooldown + recall, or an NPC re-spawned
        // by a game event). Every installed fake lives on the now-freed body and is GONE. The freshly-streamed body
        // wears vanilla gear. A rehydrate of the stale snapshot here makes apply_all_transmog see preset==last_ids and
        // real==lastReal, fire its "no state change, skipping" early-out, and leave the body un-transmogged. Wipe the
        // snapshot instead so the re-apply runs from a clean slate - mirrors the load-detect "scene graph is fresh
        // after reload" reset in load_detect_thread_fn (old fake meshes are gone even though the IDs do not change).
        const auto idx = CDCore::character_idx_from_name(name);
        const auto prev_ccoia = applied_ccoia_for_char(idx);
        const bool body_reallocated = (ccoia != prev_ccoia);
        if (body_reallocated)
        {
            if (prev_ccoia != 0)
                logger.info(
                    "[multi-apply] {} body reallocated (ccoia 0x{:X} -> "
                    "0x{:X}); wiping stale apply-cache and re-applying from clean slate",
                    name,
                    static_cast<std::uint64_t>(prev_ccoia),
                    static_cast<std::uint64_t>(ccoia)
                );
            reset_applied_state_for_char(idx);
        }
        else
        {
            rehydrate_applied_state_for_char(idx);
        }

        bool faulted = false;
        __try
        {
            apply_all_transmog(static_cast<__int64>(equip_slot));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faulted = true;
        }
        if (faulted)
        {
            // SEH during apply_all_transmog - the carrier byte patches may have landed but the mesh-rebuild path hit a
            // torn deref on a freshly-summoned body. Report as deferred so the bounded retry catches the next attempt
            // once the body has fully settled.
            logger.debug(
                "[multi-apply] {} a1=0x{:X}: exception during apply_all_transmog - deferring retry",
                name,
                static_cast<std::uint64_t>(equip_slot)
            );
            return false;
        }
        capture_applied_state_for_char(idx);
        // Record the body this apply targeted. A later roster-grew pass that observes this same pointer for this char
        // skips the work (already applied). A pointer change marks the body reallocated and forces a clean re-apply.
        set_applied_ccoia_for_char(idx, ccoia);
        logger.debug("[multi-apply] {} a1=0x{:X}", name, static_cast<std::uint64_t>(equip_slot));
        return true;
    }

    // Iterate the 1-3 live player CCOIAs from CDCore's static-chain snapshot and apply each character's preset against
    // its own equip-slot. Order: idle protagonists first, currently-controlled last, so PresetManager ends bound to the
    // controlled axis (which the editing UI and subsequent single-char applies key on). Bounded retry counter for
    // partial-chain re-arms. The world_generation() bump fires when the engine reallocates
    // Kliff's CCOIA, but the ChildContainer / actor-list chain used to enumerate Damiane and Oongka is wired separately
    // and lags by a few hundred ms during cold-load. The first debounced attempt can therefore observe Kliff present
    // but the actor list still empty (snapshot_body_cache returns only Kliff), and on a save genuinely loaded between
    // save points Kliff itself may transiently re-null between the generation bump and the worker firing. In both cases
    // the worker re-arms the schedule for another attempt. This counter caps the loop to ~30 s of retries
    // (MULTI_CHAR_RETRY_MS * MULTI_CHAR_MAX_RETRIES) so a save that never publishes Damiane or Oongka cannot pin the
    // apply worker forever.
    static std::atomic<int> s_multi_char_retry_count{0};
    static constexpr int MULTI_CHAR_MAX_RETRIES = 30;
    static constexpr std::uint64_t MULTI_CHAR_RETRY_MS = 1000;

    // Per-world-generation, per-character applied-CCOIA tracking. Cold-load and save-load bump world_generation(), at
    // which point the tracking resets and every visible player gets re-applied. In-session snapshot-grew triggers (a
    // follower summoned mid-session) only re-apply for a character whose CURRENT body pointer differs from the one
    // the last successful apply used - that skips a second apply_all_transmog on the controlled char (who was
    // already applied at cold-load) after the user summons a companion.
    //
    // Keyed by character index (1=Kliff, 2=Damiane, 3=Oongka), not a flat set of applied pointers. Indexing by
    // character is what makes a body reallocation observable: when a companion despawns (off-screen stream-out, injury
    // cooldown) and respawns, the engine hands its CCOIA back at a NEW pointer. Comparing the live pointer against the
    // per-character record distinguishes "same body, already applied" (skip) from "body reallocated, needs re-apply"
    // (wipe the stale snapshot in apply_for_one_char and re-run). A flat append-keyed pointer set cannot tell those
    // apart, and has no per-generation bound on distinct pointers, so a respawn pushes a fourth entry past the
    // three-slot array.
    static std::uint64_t s_last_applied_world_gen = 0;
    static std::array<std::uintptr_t, 3> s_applied_ccoia_for_char{};

    [[nodiscard]] static std::uintptr_t applied_ccoia_for_char(std::uint32_t idx) noexcept
    {
        if (idx < 1 || idx > 3)
            return 0;
        return s_applied_ccoia_for_char[idx - 1];
    }

    static void set_applied_ccoia_for_char(std::uint32_t idx, std::uintptr_t ccoia) noexcept
    {
        if (idx < 1 || idx > 3)
            return;
        s_applied_ccoia_for_char[idx - 1] = ccoia;
    }

    // Move the PresetManager axis back to the controlled character WITHOUT running apply_all_transmog. Used after the
    // idle pass when the controlled char was already applied earlier (so the pass restores the editing/UI axis but not
    // redo the work).
    //
    // The applied-state globals MUST be rehydrated from the controlled char's per-body snapshot, not wiped. The prior
    // multi-apply pass captured the controlled char's applied ids into its bucket, and the UI's pending-changes diff
    // compares slot_mappings (reloaded from the preset) against last_applied_ids. A zero here leaves staged !=
    // last_ids for every populated slot, which surfaces a stale "[PENDING - click Apply All]" badge for transmog that
    // is in fact already on the body.
    static void rebind_preset_to_controlled(const std::string &controlled_name) noexcept
    {
        if (controlled_name.empty())
            return;
        auto &pm = PresetManager::instance();
        if (pm.active_character() == controlled_name)
            return;
        pm.set_active_character(controlled_name);
        for (auto &m : slot_mappings())
        {
            m.active = false;
            m.target_item_id = 0;
        }
        pm.apply_to_state();
        const auto idx = CDCore::character_idx_from_name(controlled_name);
        rehydrate_applied_state_for_char(idx);
    }

    static void do_multi_char_apply() noexcept
    {
        auto &logger = DMK::log();

        // Gate on item-catalog readiness. Cold-load completes the CCOIA chain (~500 ms after world_generation bumps)
        // well before the iteminfo background scan finishes the catalog (~3 s on cold-load). Running apply_all_transmog
        // while the catalog is still empty produces slot_mappings with target_item_id=0 across the board, so
        // `apply_all_transmog` writes nothing - silently dropping every protagonist's saved preset. Hold the schedule
        // here without consuming the retry budget so a slow catalog build does not exhaust the attempt cap.
        if (!ItemNameTable::instance().ready())
        {
            logger.info("[multi-apply] catalog not ready - re-arming (retry budget preserved)");
            s_multi_char_apply_pending.store(true, std::memory_order_release);
            schedule_transmog_ms(MULTI_CHAR_RETRY_MS);
            return;
        }

        // Diagnostic dump of the raw actor list. Logged at trace so a default-level config is silent. The structured
        // `summary` is ALSO consumed by the retry oracle below (summary.actor_list distinguishes "chain not yet wired
        // through to the actor list" from "chain reached the list, save only has Kliff").
        CDCore::ActorListDebugSummary summary{};
        {
            std::array<CDCore::ActorListDebugEntry, 24> raw_entries{}; // filled for the count only
            summary = CDCore::debug_enumerate_actor_list(raw_entries.data(), raw_entries.size());
            logger.trace(
                "[multi-apply-diag] chain mgr=0x{:X} ua=0x{:X} "
                "sub=0x{:X} kliff=0x{:X} ctrl=0x{:X} vec=0x{:X} child=0x{:X} list=0x{:X} rawEntries={}",
                static_cast<std::uint64_t>(summary.mgr),
                static_cast<std::uint64_t>(summary.user_actor),
                static_cast<std::uint64_t>(summary.sub_mgr),
                static_cast<std::uint64_t>(summary.kliff_ccoia),
                static_cast<std::uint64_t>(summary.controlled),
                static_cast<std::uint64_t>(summary.vec_data),
                static_cast<std::uint64_t>(summary.child_container),
                static_cast<std::uint64_t>(summary.actor_list),
                summary.raw_entries
            );
            // Deliberately no per-entry dump: it reprints every actor on every apply, and the summary line above
            // already carries what the retry oracle reads. Add one back only while chasing the actor list itself.
        }

        std::array<CDCore::BodyCacheEntry, 3> entries{};
        const auto n = CDCore::snapshot_body_cache(entries.data(), entries.size());

        // Two partial-chain races to retry past:
        //   (1) sub-manager exists but its +0x30 (Kliff) / +0x38
        //       (controlled) slots are still NULL - snapshot returns 0.
        //   (2) Kliff/controlled wired but ClientUserActor+0x90 (vec)
        //       / ChildContainer / actor-list chain is not
        //       populated yet - snapshot returns 1 (Kliff via sub-
        //       manager+0x30), but Damiane/Oongka cannot be found.
        //
        // For case (2) the pass cannot tell apart "actor list not yet wired" from "save genuinely only has Kliff"
        // without another signal, so the loop retries up to the cap and lets MULTI_CHAR_MAX_RETRIES time out for
        // Kliff-only saves.
        const bool chain_incomplete = (n == 0) || !plausible_engine_ptr(static_cast<__int64>(summary.actor_list));
        if (chain_incomplete)
        {
            const auto attempt = s_multi_char_retry_count.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (attempt <= MULTI_CHAR_MAX_RETRIES)
            {
                logger.info(
                    "[multi-apply] partial chain (snapshot={}, actorList=0x{:X}); retrying in {} ms (attempt {}/{})",
                    n,
                    static_cast<std::uint64_t>(summary.actor_list),
                    MULTI_CHAR_RETRY_MS,
                    attempt,
                    MULTI_CHAR_MAX_RETRIES
                );
                s_multi_char_apply_pending.store(true, std::memory_order_release);
                schedule_transmog_ms(MULTI_CHAR_RETRY_MS);
                return;
            }
            logger.warning(
                "[multi-apply] gave up after {} attempts - "
                "chain never finished wiring (snapshot={}, actorList=0x{:X}); applying what we have",
                MULTI_CHAR_MAX_RETRIES,
                n,
                static_cast<std::uint64_t>(summary.actor_list)
            );
            s_multi_char_retry_count.store(0, std::memory_order_release);
            // Fall through and apply whatever resolved (Kliff only, typically) so the pass does not drop the controlled
            // char's transmog.
        }
        else
        {
            // Full chain reached - reset retry counter so the next world bump (save-load) starts fresh.
            //
            // FIXME: MULTI_CHAR_MAX_RETRIES does not bound the deferred-body re-arm. This store zeroes the counter
            // that branch reads, so a body that never becomes ready re-arms at MULTI_CHAR_RETRY_MS forever. The two
            // failures need separate counters.
            s_multi_char_retry_count.store(0, std::memory_order_release);
        }
        if (n == 0)
            return;
        // Diagnostic: dump filtered snapshot.
        for (std::size_t i = 0; i < n; ++i)
        {
            logger.trace(
                "[multi-apply-diag] snapshot[{}] ccoia=0x{:X} charIdx={}",
                i,
                static_cast<std::uint64_t>(entries[i].body),
                entries[i].char_idx
            );
        }

        const auto controlled_ccoia = CDCore::current_controlled_ccoia();

        // Reset the applied-CCOIA set whenever the world generation changes (cold-load / save-load) so the next pass
        // re-applies every player. On snapshot-grew triggers within the same generation the pass preserves the set so
        // already-applied chars are skipped.
        const auto cur_world_gen = CDCore::world_generation();
        if (cur_world_gen != s_last_applied_world_gen)
        {
            s_applied_ccoia_for_char.fill(0);
            s_last_applied_world_gen = cur_world_gen;
        }

        // First pass: idle characters. Skip any CCOIA already applied since the last world-gen bump.
        std::size_t applied = 0;
        std::size_t deferred = 0;
        std::size_t skipped = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (entries[i].body == controlled_ccoia)
                continue;
            // Skip only when THIS body (same pointer) was already applied during this world generation. A reallocated
            // body (despawn/respawn) carries a new pointer != the recorded one, so it correctly falls through to a
            // re-apply rather than being treated as "already done".
            if (entries[i].body != 0 && applied_ccoia_for_char(entries[i].char_idx) == entries[i].body)
            {
                ++skipped;
                continue;
            }
            const auto ch_enum = static_cast<CDCore::ControlledCharacter>(entries[i].char_idx);
            const auto name = std::string(CDCore::controlled_character_name(ch_enum));
            // apply_for_one_char records the applied CCOIA on success (and wipes the stale snapshot when the body was
            // reallocated), so no explicit mark step is needed here.
            if (apply_for_one_char(name, entries[i].body))
                ++applied;
            else
                ++deferred;
        }

        // Second pass: controlled character. Two cases:
        //   - First multi-apply of this world generation: no apply landed on controlled yet, so apply now and mark.
        //   - Subsequent re-fire (e.g., snapshot grew because of a summon): controlled was already applied at
        //     cold-load. Skip the apply entirely and rebind the PresetManager axis back to controlled so the editing
        //     UI / next hook-driven apply observe the correct character.
        std::string controlled_name;
        if (controlled_ccoia != 0)
        {
            const auto controlled_idx = CDCore::current_controlled_character_idx();
            if (controlled_idx >= 1 && controlled_idx <= 3)
            {
                controlled_name = std::string(CDCore::current_controlled_character_name());
                if (applied_ccoia_for_char(controlled_idx) != controlled_ccoia)
                {
                    // apply_for_one_char records the CCOIA on success.
                    if (apply_for_one_char(controlled_name, controlled_ccoia))
                        ++applied;
                    else
                        ++deferred;
                }
                else
                {
                    ++skipped;
                    rebind_preset_to_controlled(controlled_name);
                }
            }
        }

        logger.info(
            "[multi-apply] world-entry auto-apply complete: applied {} of {} player CCOIAs (deferred={} skipped={})",
            applied,
            n,
            deferred,
            skipped
        );

        // If any char's body was not ready, re-arm with a bounded retry. Freshly-summoned companions take ~1-3 seconds
        // to wire up their mesh container. A re-fire of the apply on a 1 s cadence catches them once
        // is_actor_apply_ready returns true. Cap at MULTI_CHAR_MAX_RETRIES so a body that never becomes ready cannot
        // pin the worker forever.
        if (deferred > 0)
        {
            const auto attempt = s_multi_char_retry_count.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (attempt <= MULTI_CHAR_MAX_RETRIES)
            {
                logger.info(
                    "[multi-apply] {} char(s) not yet ready; re-arming in {} ms (attempt {}/{})",
                    deferred,
                    MULTI_CHAR_RETRY_MS,
                    attempt,
                    MULTI_CHAR_MAX_RETRIES
                );
                s_multi_char_apply_pending.store(true, std::memory_order_release);
                schedule_transmog_ms(MULTI_CHAR_RETRY_MS);
            }
            else
            {
                logger.warning(
                    "[multi-apply] {} char(s) never became ready "
                    "after {} attempts - giving up until next world-gen / roster-grew event",
                    deferred,
                    MULTI_CHAR_MAX_RETRIES
                );
                s_multi_char_retry_count.store(0, std::memory_order_release);
            }
        }
        else
        {
            // Everyone applied - reset the counter so the next world transition starts with a fresh budget.
            s_multi_char_retry_count.store(0, std::memory_order_release);
        }
    }

    // One apply/clear pass. Pulled out of the worker body so the SEH frame does not share scope with the
    // condition-variable unique_lock (MSVC C2712 forbids __try with objects requiring unwinding). The worker never
    // calls it directly: dispatch_debounced_apply hands it to the game thread, so every engine call below
    // (SlotPopulator, SafeTearDown, the prefab-swap unlink) runs inside the frame, on the thread the engine mutates
    // its scene graph from.
    static void run_debounced_apply() noexcept
    {
        auto &logger = DMK::log();

        // Multi-character world-entry auto-apply takes priority over any pending single-char clear/apply. It iterates
        // every live protagonist CCOIA from CDCore::snapshot_body_cache(), applying each character's preset against its
        // own equip-slot, and ends with PresetManager bound to the controlled char so the rest of the pipeline (UI
        // editing, hook-driven applies) observes the correct axis. Early-return skips the controlled-only path below
        // because do_multi_char_apply() already applied the controlled character last.
        if (s_multi_char_apply_pending.exchange(false, std::memory_order_acq_rel))
        {
            do_multi_char_apply();
            s_last_apply_ok.store(true, std::memory_order_release);
            return;
        }

        // Consume the clear flag before resolving a1 so a racing manual_clear call is not lost.
        const bool do_clear = clear_pending().exchange(false, std::memory_order_acq_rel);

        // Targeted-apply redirect (overlay-UI initiated). When the user has the dropdown pinned to a non-controlled
        // character and `flag_apply_to_editing` is on, the overlay entry points stash that character's 1-based idx
        // here. The apply path re-resolves the CCOIA from the live snapshot (the body may have been dismissed between
        // schedule and run) and apply the editing character's preset to it. Engine-triggered hooks never set this idx,
        // so VEC / BatchEquip events continue to land on the controlled body via the default path below.
        const auto targeted_idx = s_targeted_apply_char_idx.exchange(0, std::memory_order_acq_rel);
        if (targeted_idx >= 1 && targeted_idx <= 3)
        {
            std::array<CDCore::BodyCacheEntry, 3> entries{};
            const auto n = CDCore::snapshot_body_cache(entries.data(), entries.size());
            std::uintptr_t target_ccoia = 0;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (entries[i].char_idx == targeted_idx)
                {
                    target_ccoia = entries[i].body;
                    break;
                }
            }
            if (target_ccoia == 0)
            {
                // Editing character is no longer live (follower dismissed between schedule and run, or never was
                // loaded). The user opted out of cross-body apply via the flag, so skip silently rather than render
                // their preset on the controlled body.
                logger.info("[targeted-apply] editing char idx={} not in live snapshot - skipping apply", targeted_idx);
                s_last_apply_ok.store(true, std::memory_order_release);
                return;
            }
            const auto target_a_1 = CDCore::equip_slot_for_ccoia(target_ccoia);
            if (target_a_1 == 0)
            {
                logger.info(
                    "[targeted-apply] editing char idx={} ccoia=0x{:X} "
                    "has no equip-slot yet (body still wiring) - re-arming",
                    targeted_idx,
                    static_cast<std::uint64_t>(target_ccoia)
                );
                s_targeted_apply_char_idx.store(targeted_idx, std::memory_order_release);
                schedule_transmog_ms(200);
                return;
            }

            // Per-body state hydrate. Loads this character's last-applied snapshot into the globals so Phase A's
            // teardown decisions ("for slots inactive in the new preset but active in the previous, tear down the
            // installed fake") are made against the items currently on THIS body, not whichever body was applied last.
            // Without this, a preset swap on a non-controlled char either leaks old items (no teardown) or faults
            // (teardown against the wrong body).
            rehydrate_applied_state_for_char(targeted_idx);

            // Consume the single-slot index the same way the default path does. The color-override reinit ("Reload"
            // button) schedules via manual_apply_slot so the worker reaches `apply_single_slot_transmog`, which has the
            // reinit-teardown branch that suppresses the real-item restore during the m.active=false intermediate
            // state. Routing unconditionally through apply_all_transmog (as this path previously did) walks the full
            // pipeline and re-renders the real armor for one frame, causing a visible flash on cross-body reload.
            // Mirror the default path's dispatch so single-slot, all-slot, and clear operations all behave the same
            // regardless of which body they target.
            const auto slot_idx = pending_slot_index().exchange(SLOT_COUNT, std::memory_order_acq_rel);

            bool faulted = false;
            __try
            {
                if (do_clear)
                    clear_all_transmog(static_cast<__int64>(target_a_1));
                else if (slot_idx < SLOT_COUNT)
                    apply_single_slot_transmog(static_cast<__int64>(target_a_1), slot_idx);
                else
                    apply_all_transmog(static_cast<__int64>(target_a_1));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                faulted = true;
            }
            if (faulted)
            {
                s_last_apply_ok.store(false, std::memory_order_release);
                logger.debug(
                    "[targeted-apply] exception during {} on idx={} a1=0x{:X}",
                    do_clear ? "clear" : "apply",
                    targeted_idx,
                    static_cast<std::uint64_t>(target_a_1)
                );
            }
            else
            {
                // Capture the post-apply globals back into this character's bucket so the next apply sees the
                // up-to-date installed state.
                capture_applied_state_for_char(targeted_idx);
                s_last_apply_ok.store(true, std::memory_order_release);
                logger.debug(
                    "[targeted-apply] {} idx={} slotIdx={} a1=0x{:X}",
                    do_clear                ? "cleared"
                    : slot_idx < SLOT_COUNT ? "applied-slot"
                                            : "applied",
                    targeted_idx,
                    slot_idx,
                    static_cast<std::uint64_t>(target_a_1)
                );
            }
            return;
        }

        // Prefer the hook-captured a1 from player_a1() - it identifies the character whose equip event triggered this
        // apply. Fall back to resolve_player_component() only when player_a1 is empty or unreadable (post-reload /
        // loading screen). The SEH-guarded actor deref below catches a stale wrapper after world reload by dropping the
        // apply.
        //
        // Apply ALWAYS targets the controlled character's body. The helper below syncs PresetManager's controlled axis
        // + slot_mappings to the live character. It lives outside this __try-containing function so std::string
        // destructors do not trip MSVC C2712. When the user has the overlay dropdown pinned to a different editing
        // character, the controlled body still drives carriers and the live source wrappers. The editing character only
        // supplies preset itemIds (cross-body apply).
        //
        // The helper returns false while load-detect has an unfired char swap parked in its settle window. In that case
        // the engine has not yet rotated user+0xD8 to the new body, so an apply here paints the incoming preset
        // onto the previous body. Re-arm the debounce. The load-detect commit flips the controlled axis once the
        // candidate settles.
        if (!sync_active_char_to_live())
        {
            schedule_transmog_ms(200);
            return;
        }
        __int64 a1 = player_a1().load(std::memory_order_acquire);
        if (!plausible_engine_ptr(a1))
        {
            a1 = resolve_player_component();
            if (!plausible_engine_ptr(a1))
                return;
        }

        // Verify the wrapper still points to a live actor. If the world reloaded after the last hook capture, *(a1+8)
        // faults or yields garbage - fall back to the WS chain in that case. The guarded read reports the fault as a
        // zero, so one branch covers both a torn read and an implausible actor pointer.
        const auto live_actor =
            DMK::memory::read<std::uintptr_t>(DMK::Address{static_cast<std::uintptr_t>(a1)}.offset(8)).value_or(0);
        if (!plausible_engine_ptr(static_cast<__int64>(live_actor)))
        {
            a1 = resolve_player_component();
            if (!plausible_engine_ptr(a1))
                return;
        }

        // Consume the single-slot index. SLOT_COUNT means "all slots".
        const auto slot_idx = pending_slot_index().exchange(SLOT_COUNT, std::memory_order_acq_rel);

        // Hydrate the globals with the controlled character's per-body snapshot. The default path applies on the
        // controlled body (whether the hook fired naturally or the user invoked manual_-apply without the
        // targeted-redirect flag), so Phase A needs the controlled char's installed-state to make correct teardown
        // decisions when the user swaps presets back-and-forth across protagonists.
        const auto controlled_idx = CDCore::current_controlled_character_idx();

        // Same body-reallocation check the multi-apply path makes. Switching protagonists hands back a FRESH body
        // wearing vanilla gear, so the snapshot describes fakes that live on the freed body and are gone. Rehydrating
        // it makes the tear-down gate believe a fake is already installed for a slot, skip the tear-down that a fresh
        // body needs, and leave the real part under the transmog.
        const auto controlled_ccoia = CDCore::current_controlled_ccoia();
        const auto prev_controlled_ccoia = applied_ccoia_for_char(controlled_idx);
        if (controlled_ccoia != 0 && controlled_ccoia != prev_controlled_ccoia)
        {
            if (prev_controlled_ccoia != 0)
                logger.info(
                    "[apply] controlled body reallocated (ccoia 0x{:X} -> 0x{:X}); wiping stale apply-cache",
                    static_cast<std::uint64_t>(prev_controlled_ccoia),
                    static_cast<std::uint64_t>(controlled_ccoia)
                );
            reset_applied_state_for_char(controlled_idx);
        }
        else
        {
            rehydrate_applied_state_for_char(controlled_idx);
        }

        bool faulted = false;
        __try
        {
            if (do_clear)
            {
                clear_all_transmog(a1);
                logger.debug("Transmog cleared (debounced)");
            }
            else if (slot_idx < SLOT_COUNT)
            {
                apply_single_slot_transmog(a1, slot_idx);
                logger.debug("Transmog applied slot={} (debounced)", slot_idx);
            }
            else
            {
                apply_all_transmog(a1);
                logger.debug("Transmog applied (debounced)");
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faulted = true;
        }
        if (faulted)
        {
            s_last_apply_ok.store(false, std::memory_order_release);
            // Expected during load-retry window (game visual state not ready yet). Logged at debug to avoid false-alarm
            // noise.
            logger.debug("Transmog {} exception (debounced)", do_clear ? "clear" : "apply");
        }
        else
        {
            capture_applied_state_for_char(controlled_idx);
            if (controlled_ccoia != 0)
                set_applied_ccoia_for_char(controlled_idx, controlled_ccoia);
            s_last_apply_ok.store(true, std::memory_order_release);
        }
    }

    /// game_thread job adapter: the apply body takes no context.
    static void run_debounced_apply_job(void * /*context*/) noexcept
    {
        run_debounced_apply();
    }

    // Longest the worker waits for a frame to pick an apply up before it withdraws the job and re-arms. One frame is
    // the norm; the bound only matters while no frame runs (a load hitch, a minimized window) and it keeps the worker
    // responsive to shutdown.
    static constexpr std::uint32_t GAME_THREAD_CLAIM_TIMEOUT_MS = 2000;

    // Re-arm delay after a withdrawn apply. Short enough that the apply lands within a few frames once frames resume.
    static constexpr std::uint64_t GAME_THREAD_RETRY_MS = 200;

    // Runs one apply pass on the game thread and blocks until it has finished, so the worker keeps its one-apply-at-
    // a-time contract. A pass no frame claimed did not run at all (nothing is half-applied), so it is simply
    // re-armed. Without a frame hook the pass runs here, on the worker, as it did before game_thread existed; that
    // reopens the claim-erase race window (see game_thread.hpp), so the log says so once.
    static void dispatch_debounced_apply() noexcept
    {
        using game_thread::RunResult;
        static std::atomic<bool> s_inline_warned{false};

        switch (game_thread::run_blocking(&run_debounced_apply_job, nullptr, GAME_THREAD_CLAIM_TIMEOUT_MS))
        {
        case RunResult::Ran:
        case RunResult::Shutdown:
            return;
        case RunResult::Timeout:
            DMK::log().debug(
                "[game-thread] apply not claimed by a frame within {} ms; re-arming",
                GAME_THREAD_CLAIM_TIMEOUT_MS
            );
            schedule_transmog_ms(GAME_THREAD_RETRY_MS);
            return;
        case RunResult::Unavailable:
            if (!s_inline_warned.exchange(true, std::memory_order_acq_rel))
            {
                DMK::log().warning(
                    "[game-thread] frame hook unavailable; running applies on the worker thread "
                    "(claim-erase race window open)"
                );
            }
            run_debounced_apply();
            return;
        }
    }

    // Persistent debounce worker. Sleeps on a condition variable until schedule_transmog bumps the deadline, then waits
    // out any remaining debounce window before handing run_debounced_apply to the game thread.
    static void apply_worker_fn(std::stop_token stop) noexcept
    {
        std::unique_lock<std::mutex> lk(s_apply_cv_mtx);
        for (;;)
        {
            s_apply_cv.wait(
                lk,
                []
                {
                    return shutdown_requested().load(std::memory_order_acquire) ||
                           s_apply_pending.load(std::memory_order_acquire);
                }
            );
            if (shutdown_requested().load(std::memory_order_acquire))
                return;

            // Wait until the deadline expires, picking up any later re-schedules as they arrive by looping on wait_for.
            //
            // Every new request pushes the deadline out (see schedule_transmog_ms), so during a run of fast preset
            // switches this loop keeps re-waiting and never reaches the apply. Once the switching stops, ONE apply
            // runs and it reads live state - so the preset finally landed on is the only one built.
            for (;;)
            {
                const std::uint64_t deadline = s_apply_deadline_tick.load(std::memory_order_acquire);
                const std::uint64_t now = GetTickCount64();
                if (now >= deadline)
                    break;

                const auto wait_for = std::chrono::milliseconds(deadline - now);
                s_apply_cv.wait_for(
                    lk,
                    wait_for,
                    [&deadline]
                    {
                        return shutdown_requested().load(std::memory_order_acquire) ||
                               s_apply_deadline_tick.load(std::memory_order_acquire) != deadline;
                    }
                );
                if (shutdown_requested().load(std::memory_order_acquire))
                    return;
            }

            s_apply_pending.store(false, std::memory_order_release);
            lk.unlock();

            dispatch_debounced_apply();

            lk.lock();
        }
    }

    void schedule_transmog_ms(std::uint64_t debounce_ms)
    {
        const std::uint64_t now = GetTickCount64();

        // Widen the window when this request lands close behind the previous one. A lone action keeps its short
        // debounce and applies right away. A run of them (a click down a preset list) keeps the deadline moving out
        // by the wider window, so nothing is built until the clicking stops and only the final selection applies.
        //
        // Keyed off the previous REQUEST, not the previous apply: during a burst no apply is running, so an
        // apply-based test decides the burst ended and fires mid-run - which is what let every preset
        // through before.
        const std::uint64_t prev_request = s_last_request_tick.exchange(now, std::memory_order_acq_rel);
        const bool in_burst = prev_request != 0 && now - prev_request < BURST_COALESCE_MS;
        if (in_burst && debounce_ms < BURST_COALESCE_MS)
            debounce_ms = BURST_COALESCE_MS;

        {
            std::lock_guard<std::mutex> lk(s_apply_cv_mtx);
            s_apply_deadline_tick.store(now + debounce_ms, std::memory_order_release);
            s_apply_pending.store(true, std::memory_order_release);
        }
        s_apply_cv.notify_all();
    }

    void schedule_transmog(__int64 /*a1*/, std::uint16_t /*targetId*/)
    {
        schedule_transmog_ms(APPLY_DEBOUNCE_MS);
    }

    void set_targeted_apply_char_idx(std::uint32_t char_idx) noexcept
    {
        s_targeted_apply_char_idx.store(char_idx, std::memory_order_release);
    }

    std::uint32_t pending_targeted_apply_char_idx() noexcept
    {
        return s_targeted_apply_char_idx.load(std::memory_order_acquire);
    }

    void ensure_apply_worker_started()
    {
        bool expected = false;
        if (!s_apply_worker_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;
        s_apply_worker.emplace("LtApplyWorker", &apply_worker_fn);
    }

    void stop_apply_worker()
    {
        if (!s_apply_worker_started.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard<std::mutex> lk(s_apply_cv_mtx);
            s_apply_pending.store(true, std::memory_order_release);
        }
        // request_stop() before the notify so the body observes the stop signal on the wake it is about to get:
        // the apply worker parks on a condition variable, which a stop request alone does not disturb.
        if (s_apply_worker.has_value())
            s_apply_worker->request_stop();
        s_apply_cv.notify_all();
        s_apply_worker.reset();
        s_apply_worker_started.store(false, std::memory_order_release);
    }

    // Load-detection thread

    static std::optional<DMK::StoppableWorker> s_load_detect_worker;

    // Guarded walk of WorldSystem -> ActorManager -> UserActor. The UserActor pointer is stable for the lifetime of a
    // save: the singleton itself is never swapped when the player changes which body they control (only user+0xD8
    // rotates between actor slots). A change of the UserActor pointer is therefore a reliable signal for "new world
    // loaded" - distinct from an in-session character swap, which does NOT reallocate this singleton.
    //
    // Returns 0 on any fault or on an unresolved chain. Callers treat 0 as "chain not yet ready" and defer the
    // save-load branch.
    static std::uintptr_t read_user_actor_ptr() noexcept
    {
        // Named entry point over the shared walker (see walk_ws_to_user_actor). Kept distinct because callers read it
        // specifically as the world-reload signal documented above (UserActor pointer change == new world loaded).
        return walk_ws_to_user_actor();
    }

    /**
     * @brief Guarded read of `user+0xD8` (the rotating CLIENT body pointer that EH watches as its save-load signal).
     * @details Mirrors the EquipHide player-detection probe so LT detects the same X->0->Y and atomic-swap save-load
     *          transitions EH does and clears preset_manager.active_character() on the wipe paths. Returns 0 on any
     *          fault, on an unresolved chain, or when the engine publishes the controlled body as null during a
     *          transition.
     */
    static std::uintptr_t read_controlled_actor_ptr() noexcept
    {
        const auto user = walk_ws_to_user_actor();
        if (!plausible_engine_ptr(static_cast<__int64>(user)))
            return 0;

        // user+0xD8 is the CLIENT body pointer (rotates per radial swap or save-load arena allocation). Unlike the
        // chain reads in the shared walker, this path DOES NOT reject 0, because an X->0 transition is the save-load
        // signal the detector wants.
        return DMK::memory::read<std::uintptr_t>(DMK::Address{user}.offset(USER_TO_CONTROLLED)).value_or(0);
    }

    /**
     * @brief Settle window for the character-swap detector.
     * @details During world load the engine rotates `user+0xD8` through the party members as it wires each actor
     *           (e.g. Kliff -> Oongka -> Damiane on a Damiane save). Each rotation is a genuine engine state, so the
     *           resolver reports the transient identities. An immediate swap applies the
     *           wrong character's preset and costs ~3x wasted apply work plus a brief visual flicker. The window holds
     *           the new identity until it stays stable, which collapses the load-time churn into a single transition
     *           and still propagates a real user-initiated swap after a 1s delay.
     */
    static constexpr std::uint64_t CHAR_SWAP_SETTLE_MS = 1000;

    /**
     * @brief Walks the live actor chain once and republishes the protagonist body-ownership table from it.
     * @details This thread is the table's only producer, so wherever this thread parks the table stops being
     *          refreshed. That matters most during a world load, which is exactly when its rows are shortest and
     *          likeliest to name bodies the engine has already replaced, so the auto-apply retry loop refreshes
     *          through here as well as the steady-state tick.
     *
     *          The snapshot is split into parallel arrays because shared_state.hpp deliberately carries no CDCore
     *          include. See @ref publish_body_owner_table.
     * @return Number of protagonists the snapshot found, which doubles as the roster sample the caller compares
     *         against to detect a companion arriving.
     */
    static std::size_t publish_owner_table_from_live() noexcept
    {
        std::array<CDCore::BodyCacheEntry, BODY_OWNER_CAP> snap{};
        const auto found = CDCore::snapshot_body_cache(snap.data(), snap.size());

        std::array<std::uintptr_t, BODY_OWNER_CAP> ccoias{};
        std::array<std::uint32_t, BODY_OWNER_CAP> char_idxs{};
        for (std::size_t i = 0; i < found; ++i)
        {
            ccoias[i] = snap[i].body;
            char_idxs[i] = snap[i].char_idx;
        }
        publish_body_owner_table(ccoias.data(), char_idxs.data(), found);
        return found;
    }

    static void load_detect_thread_fn(std::stop_token stop) noexcept
    {
        auto &logger = DMK::log();
        __int64 prev_comp = 0;
        std::uintptr_t prev_user = 0;
        std::string prev_char_name;

        // Settle-window state for the swap detector below. pending_char_name is empty when no candidate is in flight.
        // otherwise it holds the candidate name observed since pending_first_seen_tick. The swap commits only when (now
        // - pending_first_seen_tick) >= settle window.
        std::string pending_char_name;
        std::uint64_t pending_first_seen_tick = 0;

        // Save-load detection state. Tracks the CLIENT body pointer (user+0xD8) across ticks. An X->0 transition
        // latches pending_reload_invalidation. The following 0->Y (with the flag set) OR a non-zero atomic X->Y
        // (disambiguated below via CDCore::world_generation()) triggers the wipe.
        std::uintptr_t prev_controlled_actor = 0;
        bool pending_reload_invalidation = false;
        // CDCore world-generation tracking: bumps when the engine reallocates Kliff's CCOIA (cold-load or save-load).
        // Drives two consumers in this loop:
        //   1. atomic-X->Y disambiguation in the controlled-actor block below (in-session radial swap leaves Kliff's
        //      CCOIA pointer unchanged, so the generation stays flat).
        //   2. multi-character world-entry auto-apply at the top of every loop iteration - on any bump the load thread
        //      arms s_multi_char_apply_pending so the apply worker iterates snapshot_body_cache() and stamps each
        //      protagonist's preset against its own equip-slot.
        //
        // It starts at 0 so the very first observation reads as a bump and fires the multi-char auto-apply without
        // needing a save-load to bootstrap.
        std::uint64_t prev_world_gen = 0;

        while (!shutdown_requested().load(std::memory_order_relaxed))
        {
            sleep_interruptible(stop, 1000);
            if (stop.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                return;

            // Multi-character auto-apply triggers
            //
            // Two independent signals re-arm the multi-apply path:
            //
            //   (1) CDCore::world_generation() bumps when the engine reallocates Kliff's CCOIA (cold-load or
            //       save-load). It starts at 0 so the very first iteration after thread start fires (cold-load or
            //       LT loaded into a live world).
            //
            //   (2) Player snapshot grew. Kliff's CCOIA is stable across mid-session "summons" (e.g. a UI action
            //       that spawns Damiane or Oongka), so world_generation alone misses these. The tick polls
            //       snapshot_body_cache. When it returns more player CCOIAs than before, the path re-arms. The
            //       prev_snapshot_count guard keeps a fresh world (0 -> N) from a double-fire alongside the
            //       world-gen path.
            //
            // prev_world_gen is updated by the controlled-actor block below (which also reads cur_world_gen for its
            // atomic-X->Y disambiguation), so this path does not write it.
            //
            // The snapshot-count sentinel uses static_cast<size_t>(-1) to mean "uninitialized" (never observed). This
            // is semantically distinct from "observed 0", which the previous `> 0` guard conflated. The world-gen path
            // resets it to uninitialized so the first post-bump iteration records the snapshot count without
            // re-firing (world-gen already armed the apply).
            static constexpr std::size_t k_snapshot_count_uninit = static_cast<std::size_t>(-1);
            static std::size_t s_prev_snapshot_count = k_snapshot_count_uninit;
            {
                // Refresh the ownership table first, so the socket-build detour on the engine threads is answering
                // from this tick's roster rather than the previous one. The count doubles as the roster sample the
                // watcher below compares against, so the walk happens once.
                const auto n = publish_owner_table_from_live();

                const auto cur_world_gen = CDCore::world_generation();
                if (cur_world_gen != prev_world_gen)
                {
                    logger.info(
                        "World generation {} -> {}; arming multi-char auto-apply (500 ms debounce)",
                        prev_world_gen,
                        cur_world_gen
                    );
                    s_multi_char_apply_pending.store(true, std::memory_order_release);
                    // Tight first-shot: apply_for_one_char treats a mesh-walk fault as a deferred retry (re-arms
                    // s_multi_char_apply_pending and returns false) so the bounded retry below picks it up if the body
                    // is not fully wired yet. Net cost of an early miss is one extra retry tick. Net gain when the body
                    // IS ready is 1.5s faster apply.
                    schedule_transmog_ms(500);
                    s_prev_snapshot_count = k_snapshot_count_uninit;
                }
                else
                {
                    if (s_prev_snapshot_count != k_snapshot_count_uninit && n > s_prev_snapshot_count)
                    {
                        logger.info(
                            "Player roster grew {} -> {}; arming multi-char auto-apply (500 ms debounce)",
                            s_prev_snapshot_count,
                            n
                        );
                        s_multi_char_apply_pending.store(true, std::memory_order_release);
                        // Match the world-gen debounce. The deferred-retry path covers the "newly-summoned but
                        // not-yet-wired" window.
                        schedule_transmog_ms(500);
                    }
                    s_prev_snapshot_count = n;
                }
            }

            // Save-load invalidation
            //
            // The Core controlled-character resolver caches the most recent known identity key. On a save load the
            // previous world's actor pool is freed and reallocated. Without invalidation the resolver keeps
            // returning the prior save's character until the chain walk first observes the new actor's identity u32s.
            //
            // Use the UserActor pointer as the save-load signal: the ActorManager rewrites it on world load but leaves
            // it alone during in-session character swaps (only user+0xD8 rotates on swap). Invalidating on every comp
            // change wipes the cache during normal swaps and adds a full 1s tick of latency before the char-swap
            // detector below confirms the new name.
            //
            // Runs BEFORE the char-swap block so a save-load event routes through the retry loop further down instead
            // of firing a spurious "swap detected" log against a stale cached name.
            {
                const auto cur_user = read_user_actor_ptr();
                if (cur_user != 0 && cur_user != prev_user)
                {
                    if (prev_user != 0)
                    {
                        logger.info(
                            "Load detect: UserActor swapped "
                            "({:#x} -> {:#x}); invalidating controlled-char cache for save-load transition",
                            static_cast<uint64_t>(prev_user),
                            static_cast<uint64_t>(cur_user)
                        );
                        CDCore::invalidate_controlled_character();
                        prev_char_name.clear();
                        pending_char_name.clear();
                        // Same rationale as wipe_lt_state in the controlled-actor branch below: every fake installed
                        // against the previous arena is gone, so the per-body trackers must reset before the next apply
                        // hydrates them.
                        reset_all_applied_state();
                        prefab_wrapper_swap::reset_per_char_state();
                        // Re-populate the body-mesh prefab catalog. Save loads can rotate the AppearanceTableLoader
                        // registry's resident wrapper set as zone-/ archetype-specific assets stream in/out, and the
                        // picker dropdown otherwise stays pinned to the boot snapshot until the user clicks "Refresh
                        // Catalog" manually. Idempotent and cheap (~5ms StringInfo walk + ~10ms registry enum). Fires
                        // once per save-load tick.
                        prefab_wrapper_swap::populate_slot_catalogs();
                    }
                    prev_user = cur_user;
                }
            }

            // Save-load detected (controlled-actor signal)
            //
            // Mirrors EquipHide/player_detection.cpp. Polls user+0xD8 (the CLIENT controlled body pointer) and
            // disambiguates three transitions:
            //   1. X->0 : engine published null -> latch deferred wipe
            //   2. 0->Y with flag set : world live again -> wipe
            //   3. X->Y atomic : CDCore::world_generation() bump = the engine reallocated Kliff's CCOIA (save-load).
            //      unchanged generation = in-session radial swap.
            //
            // Every wipe path ALSO clears LT-local state that otherwise routes through the previously-active
            // character's preset (preset_manager.active_character() and the prev_char_name/pending_char_name settle
            // window). This is the "On save load detected, clear the current char of LT" requirement. An empty
            // active_character() resolves to no preset, so the engine's vanilla items show until the next chain walk
            // observes the correct identity.
            {
                const auto controlled_actor = read_controlled_actor_ptr();
                const auto cur_world_gen = CDCore::world_generation();
                if (controlled_actor != prev_controlled_actor)
                {
                    auto wipe_lt_state = [&]()
                    {
                        CDCore::invalidate_controlled_character();
                        PresetManager::instance().set_active_character("");
                        prev_char_name.clear();
                        pending_char_name.clear();
                        // The engine reallocated every actor body during this transition, so all fakes the previous
                        // session installed are gone. Drop both the globals and the per-character snapshot buffers so
                        // the next apply hydrates from a clean slate instead of trying to tear down items that no
                        // longer exist on the new arena.
                        reset_all_applied_state();
                        prefab_wrapper_swap::reset_per_char_state();
                    };

                    if (controlled_actor == 0 && prev_controlled_actor != 0)
                    {
                        logger.info(
                            "Save-load detected: controlled actor "
                            "(0x{:X} -> 0x0); deferring full cache wipe until new world is live",
                            static_cast<uint64_t>(prev_controlled_actor)
                        );
                        pending_reload_invalidation = true;
                    }
                    else if (controlled_actor != 0 && prev_controlled_actor == 0 && pending_reload_invalidation)
                    {
                        logger.info(
                            "Save-load complete: new controlled actor "
                            "0x{:X} - clearing LT preset_manager active character + swap-scope state",
                            static_cast<uint64_t>(controlled_actor)
                        );
                        wipe_lt_state();
                        pending_reload_invalidation = false;
                    }
                    else if (prev_controlled_actor != 0 && controlled_actor != 0)
                    {
                        // Atomic X->Y. World-generation bump = the engine reallocated Kliff's CCOIA during save-load.
                        // No bump = in-session radial swap. Preserve LT state.
                        const bool atomic_save_load = cur_world_gen != prev_world_gen;

                        if (atomic_save_load)
                        {
                            logger.info(
                                "Save-load detected (atomic swap): "
                                "controlled actor (0x{:X} -> 0x{:X}); "
                                "world_generation {} -> {}; clearing "
                                "LT preset_manager active character + swap-scope state",
                                static_cast<uint64_t>(prev_controlled_actor),
                                static_cast<uint64_t>(controlled_actor),
                                prev_world_gen,
                                cur_world_gen
                            );
                            wipe_lt_state();
                            pending_reload_invalidation = false;
                        }
                        // else: normal char swap - LT's existing char-swap auto-detect block (below) picks up the
                        // identity change via the live chain walk and calls set_active_character.
                    }
                    prev_controlled_actor = controlled_actor;
                }
                prev_world_gen = cur_world_gen;
            }

            // Character-swap auto-detect
            //
            // Reads the controlled-character name every tick. When it changes AND stays stable for
            // CHAR_SWAP_SETTLE_MS, switches the UI preset list to the new character and schedules an apply. The apply
            // path re-walks the WS chain through user+0xD8 so it always resolves the correct per-character wrapper
            // without needing a BatchEquip event.
            //
            // The settle window absorbs load-time wiring churn where the engine briefly rotates user+0xD8 through party
            // members before settling on the save's controlled actor. See the CHAR_SWAP_SETTLE_MS declaration above for
            // the rationale.
            {
                const auto live_name = current_controlled_character_name();
                auto &pm = PresetManager::instance();

                // Branch on three states:
                //   1. live_name empty / matches prev_char_name - no transition. Clear any pending candidate so a
                //      back-and-forth (A -> B -> A within settle window)
                //      does not commit a phantom swap to B.
                //   2. live_name differs from pending_char_name - new candidate. Restart the settle clock.
                //   3. live_name matches pending_char_name - candidate held. Commit when the elapsed time crosses the
                //      settle threshold.
                if (live_name.empty() || live_name == prev_char_name)
                {
                    pending_char_name.clear();
                    s_char_swap_pending.store(false, std::memory_order_release);
                }
                else if (live_name != pending_char_name)
                {
                    pending_char_name = live_name;
                    pending_first_seen_tick = GetTickCount64();
                    s_char_swap_pending.store(true, std::memory_order_release);
                }
                else if (GetTickCount64() - pending_first_seen_tick >= CHAR_SWAP_SETTLE_MS)
                {
                    const std::string old_name = prev_char_name.empty() ? pm.active_character() : prev_char_name;
                    prev_char_name = live_name;
                    pending_char_name.clear();
                    // Clear BEFORE schedule_transmog_ms below so the scheduled run_debounced_apply observes the
                    // committed flip and does not defer again.
                    s_char_swap_pending.store(false, std::memory_order_release);

                    if (live_name != pm.active_character())
                    {
                        pm.set_active_character(live_name);
                        // An in-game controlled-character change to a body different from any prior dropdown "pin" must
                        // release that pin: the dropdown selection is treated as transient across real
                        // controlled-character swaps. Without this release, the worker feeds the pinned character's
                        // preset into slot_mappings via apply_to_state below and schedule_transmog applies it to the
                        // newly-controlled body (e.g. user pins
                        // Damiane while controlling Kliff, then swaps to Oongka - without release, Damiane's outfit
                        // lands on Oongka). set_active_character already auto-clears the pin when the new controlled
                        // char IS the pinned char. This handles the third-character case.
                        if (pm.editing_pinned() && pm.editing_character() != live_name)
                        {
                            pm.clear_editing_pin();
                        }
                        for (auto &m : slot_mappings())
                        {
                            m.active = false;
                            m.target_item_id = 0;
                        }
                        pm.apply_to_state();
                        {
                            std::lock_guard<std::mutex> lk(s_apply_cv_mtx);
                            last_applied_ids().fill(0);
                            real_damaged().fill(false);
                            last_applied_real_ids().fill(0);
                            last_applied_carrier_ids().fill(0);
                        }
                        logger.info("Char swap detected: {} -> {}", old_name, live_name);
                        if (flag_enabled().load(std::memory_order_relaxed))
                            schedule_transmog_ms(200);
                        pm.save();
                    }
                }
            }

            auto comp = resolve_player_component();

            // Detect change: new component appeared or address changed.
            if (plausible_engine_ptr(comp) && comp != prev_comp)
            {
                // Resolve identity WITHOUT invalidating any CDCore cache. The focus-broadcast resolver stamps Tier-0 on
                // every engine focus event, so a stale read here is self-correcting within one tick. Calling
                // an inline invalidate_controlled_character() forces an Unknown window on saves whose first broadcast
                // has not arrived yet (Prologue / post-cutscene resume), gating the auto-apply indefinitely.
                const std::string live_char = current_controlled_character_name();

                // Advance prev_comp BEFORE the controlled-char gate so the worker does not re-observe the same change
                // every tick while the world is still loading. prev_comp tracks the most recent observed value, not the
                // most recent successfully-applied one.
                logger.info(
                    "Load detect: player component changed ({:#x} -> {:#x}); controlled = {}",
                    static_cast<uint64_t>(prev_comp),
                    static_cast<uint64_t>(comp),
                    live_char.empty() ? std::string_view{"<unresolved>"} : std::string_view{live_char}
                );
                prev_comp = comp;

                // Engine has rotated player_component to the new body, so the race window that s_char_swap_pending
                // guards is closed regardless of whether the char-swap auto-detect block above has finished its settle
                // window. Without this clear, the retry loop below blocks the load-detect thread for several seconds,
                // the auto-detect block never re-ticks during that span, and sync_active_char_to_live keeps deferring
                // every scheduled apply - the visible symptom is "scheduled apply (attempt N)" loglines with no apply
                // ever landing after a save-load or radial swap.
                s_char_swap_pending.store(false, std::memory_order_release);

                if (live_char.empty())
                {
                    logger.trace("Load detect: holding auto-apply - controlled char unresolved (waiting for world)");
                    continue;
                }

                player_a1().store(comp, std::memory_order_release);

                // Reset cached apply state so the early-out in apply_all_transmog does not suppress the re-apply. The
                // scene graph is fresh after reload - old fake meshes are gone even though the IDs do not change.
                //
                // Held under s_apply_cv_mtx to prevent racing with an in-flight apply, which reads
                // and writes these same non-atomic arrays.
                {
                    std::lock_guard<std::mutex> lk(s_apply_cv_mtx);
                    last_applied_ids().fill(0);
                    last_applied_real_ids().fill(0);
                    last_applied_carrier_ids().fill(0);
                    real_damaged().fill(false);
                }

                if (!flag_enabled().load(std::memory_order_relaxed))
                    continue;

                // Retry through the debounce worker. The game's visual state is not ready immediately after load detect
                // - the first attempt often faults because the PartDef array and scene graph are still being
                // populated. The loop retries with exponential backoff up to ~90s total and exits once the apply lands
                // (no SEH fault).
                //
                // Backoff schedule: 2s, 2s, 3s, 4s, 5s, 5s, 5s, ... This gives fast feedback when the game loads
                // quickly but does not burn CPU during longer load screens.
                //
                // Each iteration runs a cheap actor-readiness probe (real_part_tear_down::is_actor_apply_ready) before
                // scheduling a full apply. The probe walks the same container chain that tear_down dereferences but
                // performs no engine calls. On a placeholder wrapper (engine still wiring during world load) the probe
                // returns false at microsecond cost and the iteration skips the full apply, avoiding ~5 SEH-faulted
                // tear_down log lines plus an apply fault per attempt. The 20-attempt budget is preserved for low-end
                // PCs where wiring legitimately takes >60s. The probe makes the wait silent and cheap.
                //
                // The probe is suppressed on attempt 0 so first-load fast-paths (e.g. the engine warmed before the
                // thread was scheduled) still fire an apply immediately without an extra delay.
                static constexpr int max_auto_apply_attempts = 20;
                s_last_apply_ok.store(false, std::memory_order_release);

                int not_ready_streak = 0;
                int catalog_wait_streak = 0;
                bool wrapper_changed = false;

                for (int attempt = 0; attempt < max_auto_apply_attempts; ++attempt)
                {
                    const int delay_ms = (attempt < 2) ? 2000 : (attempt < 3) ? 3000 : (attempt < 4) ? 4000 : 5000;
                    sleep_interruptible(stop, delay_ms);
                    if (stop.stop_requested() || shutdown_requested().load(std::memory_order_relaxed))
                        break;

                    // The outer tick cannot run while this loop owns the thread, and this loop can hold it for a
                    // minute or more. Refresh here too, or the engine threads spend the whole load answering from a
                    // roster captured before it started.
                    (void)publish_owner_table_from_live();

                    // Mid-retry wrapper-change abort. The engine sometimes parks user+0xD8 on a placeholder wrapper for
                    // 60+ seconds before deallocating it and allocating the real character actor at a different
                    // address. Without this check the retry loop burns the rest of its budget on the dead
                    // placeholder and only notice the new wrapper when the outer load-detect tick runs again post-loop.
                    // A re-resolve of the component here bails within one attempt's delay of the swap. The outer
                    // loop's next iteration will see comp != prev_comp and start a fresh retry budget against the new
                    // wrapper.
                    {
                        const auto cur_comp = resolve_player_component();
                        if (plausible_engine_ptr(cur_comp) && cur_comp != comp)
                        {
                            logger.info(
                                "Load detect: wrapper changed mid-retry ({:#x} -> {:#x}), aborting current budget",
                                static_cast<uint64_t>(comp),
                                static_cast<uint64_t>(cur_comp)
                            );
                            wrapper_changed = true;
                            break;
                        }
                    }

                    // Gate on item-catalog readiness. On hot reload the game is already mid-session with real equipment
                    // populated, so the "real armor changed" branch in apply_all_transmog fires tear_down before
                    // the preset has any resolved item IDs (names still pending background-thread catalog build). That
                    // strips the character and leaves slots in a state where the deferred post-resolve re-apply
                    // crashes. Hold the retry budget here until the catalog publishes. The attempt counter does not
                    // advance during the wait so a slow catalog build does not exhaust attempts. The outer
                    // wrapper-change check above still runs each tick.
                    if (!ItemNameTable::instance().ready())
                    {
                        if (catalog_wait_streak == 0)
                            logger.debug(
                                "Load detect: catalog not ready - holding auto-apply (attempt {})",
                                attempt + 1
                            );
                        ++catalog_wait_streak;
                        --attempt; // do not consume an attempt slot
                        continue;
                    }
                    if (catalog_wait_streak > 0)
                    {
                        logger.debug("Load detect: catalog became ready after {} deferred ticks", catalog_wait_streak);
                        catalog_wait_streak = 0;
                    }

                    if (attempt > 0 && !real_part_tear_down::is_actor_apply_ready(reinterpret_cast<void *>(comp)))
                    {
                        // Log once per fault streak so the deferral is visible in the log without filling it. The
                        // streak resets when a probe finally passes or when the wrapper changes (next outer-loop
                        // iteration).
                        if (not_ready_streak == 0)
                            logger.debug("Load detect: actor not ready - deferring apply (attempt {})", attempt + 1);
                        ++not_ready_streak;
                        continue;
                    }

                    if (not_ready_streak > 0)
                    {
                        logger.debug("Load detect: actor became ready after {} deferred attempts", not_ready_streak);
                        not_ready_streak = 0;
                    }

                    schedule_transmog_ms(0);
                    logger.debug("Load detect: scheduled apply (attempt {})", attempt + 1);

                    // Give the worker time to finish, then check result.
                    sleep_interruptible(stop, 500);
                    if (s_last_apply_ok.load(std::memory_order_acquire))
                    {
                        logger.info("Load detect: auto-apply succeeded on attempt {}", attempt + 1);
                        break;
                    }
                }
                // Suppress the failure warning when the loop aborted because the wrapper changed - that path is a
                // planned hand-off to the outer loop, not a failure.
                if (!wrapper_changed && !s_last_apply_ok.load(std::memory_order_acquire))
                    logger.warning("Load detect: auto-apply failed after {} attempts", max_auto_apply_attempts);
            }
            else if (plausible_engine_ptr(comp))
            {
                prev_comp = comp;
            }
        }

        return;
    }

    void start_load_detect_thread()
    {
        s_load_detect_worker.emplace("LtLoadDetect", &load_detect_thread_fn);
    }

    void stop_load_detect_thread()
    {
        // Polls via sleep_interruptible(1000), auto-apply retries up to 20x with backoff. Every sleep checks the
        // stop token and shutdown_requested every 100ms, so the drain completes well under a second.
        // ~StoppableWorker requests stop and joins, so the reset IS the join. Unlike the WaitForSingleObject
        // timeout this replaced, there is no path where the wait gives up and leaves the thread running inside a
        // module that is about to unmap.
        s_load_detect_worker.reset();
    }

} // namespace Transmog
