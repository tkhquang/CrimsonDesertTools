#include "transmog_worker.hpp"
#include "indexed_string_table.hpp"
#include "part_show_suppress.hpp"
#include "prefab_wrapper_swap.hpp"
#include "constants.hpp"
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
#include <mutex>
#include <thread>

namespace Transmog
{
    namespace
    {
        // Map PresetManager character names to the 1-based char-idx
        // CDCore::snapshot_body_cache emits (1=Kliff, 2=Damiane,
        // 3=Oongka). Returns 0 for unknown names so the per-body
        // hydrate/capture helpers no-op safely on caller mistakes.
        std::uint32_t char_idx_for_preset_name(
            const std::string &name) noexcept
        {
            if (name == "Kliff")   return 1;
            if (name == "Damiane") return 2;
            if (name == "Oongka")  return 3;
            return 0;
        }
    } // namespace

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
                if (flag_dump_item_catalog().load(std::memory_order_relaxed))
                    ItemNameTable::instance().dump_catalog_tsv();
                if (flag_dump_item_prefabs().load(std::memory_order_relaxed))
                {
                    // Targeted phantom-recovery sweep can take ~minutes.
                    // Detach so reresolve_all_names + manual_apply
                    // happen immediately and the live transmog state
                    // refreshes without waiting on the TSV write.
                    std::thread{[] { dump_itemmesh_tsv(); }}.detach();
                }

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

    // --- Deferred PartShowSuppress slot-hash scan ---
    //
    // Mirrors deferred_nametable_scan_fn but for the IndexedStringA
    // entries PartShowSuppress keys on. A synchronous scan at LT init
    // would observe a small / empty table on cold-launch (LT loaded
    // before main-menu wiring finishes), leaving PartShowSuppress
    // inert for the entire session. The deferred worker gates on
    // Transmog::is_world_ready() and waits until every expected
    // partShowHashKey resolves, then commits once.
    static std::mutex s_slotHashThreadMtx;
    static std::thread s_slotHashScanThread;
    static std::atomic<bool> s_slotHashScanLaunched{false};

    // Timings match k_nametableInitialDelayMs / k_nametableRetryMs so
    // both deferred workers behave the same way during cold-launch.
    static constexpr int k_slotHashInitialDelayMs = 8000;
    static constexpr int k_slotHashRetryMs        = 2000;

    static std::size_t expected_slot_hash_count() noexcept
    {
        std::size_t expected = 0;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const char *key = k_slotMetadata[i].partShowHashKey;
            if (key && key[0] != '\0')
                ++expected;
        }
        return expected;
    }

    static void deferred_slot_hash_scan_fn() noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        const auto mapLookupAddr = resolved_addrs().mapLookup;
        if (!mapLookupAddr)
            return; // MapLookup unresolved -- already warned at init.

        const auto expectedCount = expected_slot_hash_count();
        if (expectedCount == 0)
            return; // Nothing to resolve (no partShowHashKey rows).

        // Initial grace period before the first scan; mirrors the
        // nametable worker's initial delay so we don't waste polls
        // while the engine is still in pre-main-menu state.
        for (int slept = 0; slept < k_slotHashInitialDelayMs; slept += 250)
        {
            if (shutdown_requested().load(std::memory_order_relaxed))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        std::size_t prevResolvable = 0;

        for (int attempt = 1; ; ++attempt)
        {
            for (int slept = 0; slept < k_slotHashRetryMs; slept += 250)
            {
                if (shutdown_requested().load(std::memory_order_relaxed))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }

            // World-ready gate. Without a live world the IndexedStringA
            // table carries only a few engine-internal seed entries
            // and the CD_* part names are not yet registered.
            if (!Transmog::is_world_ready())
            {
                if (attempt % 50 == 0)
                    logger.warning(
                        "[dispatch] slot-hash deferred scan: waiting "
                        "for world after {} attempts",
                        attempt);
                continue;
            }

            auto nameToHash = scan_indexed_string_table(mapLookupAddr);
            if (nameToHash.empty())
            {
                if (attempt % 50 == 0)
                    logger.warning(
                        "[dispatch] slot-hash deferred scan: "
                        "IndexedStringA empty after world-ready "
                        "({} attempts)",
                        attempt);
                continue;
            }

            // Probe how many of our target keys are present without
            // mutating PartShowSuppress yet. Keeps the publish path
            // single-shot so the hook sees one atomic transition
            // from "inert" to "fully ready".
            std::size_t resolvable = 0;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                const char *key = k_slotMetadata[i].partShowHashKey;
                if (!key || key[0] == '\0')
                    continue;
                if (nameToHash.find(key) != nameToHash.end())
                    ++resolvable;
            }

            if (resolvable == expectedCount)
            {
                const auto resolved =
                    PartShowSuppress::init_slot_hashes(nameToHash);
                logger.info(
                    "[dispatch] slot hashes resolved via deferred "
                    "scan: {}/{} slots ({} attempts)",
                    resolved, expectedCount, attempt);
                return;
            }

            if (resolvable > prevResolvable)
            {
                logger.debug(
                    "[dispatch] slot-hash deferred scan: {}/{} "
                    "resolvable (attempt {})",
                    resolvable, expectedCount, attempt);
                prevResolvable = resolvable;
                continue;
            }

            // Plateaued -- sleep and retry indefinitely until every
            // expected key registers or shutdown is requested.
            if (attempt % 50 == 0)
                logger.warning(
                    "[dispatch] slot-hash deferred scan: still "
                    "waiting after {} attempts ({}/{} resolvable)",
                    attempt, resolvable, expectedCount);
        }
    }

    void launch_deferred_slot_hash_scan() noexcept
    {
        if (s_slotHashScanLaunched.exchange(true, std::memory_order_acq_rel))
            return;

        std::lock_guard<std::mutex> lk(s_slotHashThreadMtx);
        s_slotHashScanThread = std::thread(deferred_slot_hash_scan_fn);
    }

    void join_deferred_slot_hash_scan()
    {
        std::lock_guard<std::mutex> lk(s_slotHashThreadMtx);
        if (s_slotHashScanThread.joinable())
            s_slotHashScanThread.join();
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

    // One-shot redirect: the editing character's 1-based idx that the
    // next scheduled apply should target instead of the controlled
    // body. Set by overlay-UI entry points via
    // set_targeted_apply_char_idx() and exchange-consumed by
    // run_debounced_apply. Engine-triggered hook paths never touch this
    // atomic, so the controlled body remains the default target for
    // VEC / BatchEquip events even while the user is editing a
    // non-controlled character.
    static std::atomic<std::uint32_t> s_targetedApplyCharIdx{0};

    // Set by run_debounced_apply: true if the last apply completed
    // without SEH fault, false on exception. Used by load-detect to
    // stop retrying once a boot-time apply succeeds.
    static std::atomic<bool> s_lastApplyOk{false};

    // True while load_detect_thread_fn has a controlled-character
    // change parked in its settle window but not yet committed. The
    // settle-window branch is the authoritative committer: it flips
    // pm.active_character() and schedules the apply once the
    // candidate identity holds for k_charSwapSettleMs.
    //
    // Without this gate, sync_active_char_to_live() would flip inline
    // the moment the controlled-char probe returns the new identity,
    // but the engine may not have rotated user+0xD8 to the new actor
    // yet. A hook-driven apply that races into run_debounced_apply
    // during that window would resolve a1 to the previous body and
    // paint the new character's preset onto it (e.g. save-load on
    // Kliff that auto-toggles to Oongka leaving Kliff wearing
    // Oongka's preset). sync_active_char_to_live consults this flag
    // and returns false to defer; the caller re-arms the debounce
    // until the settle commits.
    static std::atomic<bool> s_charSwapPending{false};

    // Multi-character auto-apply request, set by the load-detect
    // thread when CDCore::world_generation() bumps (engine has
    // reallocated Kliff's CCOIA: cold-load or save-load).
    // Consumed at the top of run_debounced_apply -- the worker walks
    // CDCore::snapshot_body_cache(), then for each of the 1-3 player
    // CCOIAs swaps PresetManager's active character to that char,
    // resolves its equip-slot via CDCore::equip_slot_for_ccoia(), and
    // invokes apply_all_transmog so every protagonist gets its saved
    // preset on world entry without needing the user to cycle to them.
    static std::atomic<bool> s_multiCharApplyPending{false};

    static std::mutex s_applyCvMtx;
    static std::condition_variable s_applyCv;
    static std::thread s_applyWorker;
    static std::atomic<bool> s_applyWorkerStarted{false};

    // Apply ALWAYS targets the currently-controlled character. This
    // helper re-reads the live character via the Core resolver and,
    // when PresetManager has a different controlled character
    // cached, switches to the live one and rebuilds slot_mappings
    // from its preset. Holds no __try block so std::string usage is
    // fine; called from run_debounced_apply (which cannot mix __try
    // with C++ objects).
    //
    // The editing axis is left untouched here: if the user has the
    // overlay dropdown pinned to a different character, that pin
    // persists across in-game character swaps and the cross-body
    // apply remains in effect.
    //
    // Returns true when the apply may proceed, false when the caller
    // must defer (re-arm the debounce). False is returned only while
    // load_detect_thread_fn has an unfired char-swap parked in its
    // settle window: see s_charSwapPending.
    [[nodiscard]] static bool sync_active_char_to_live() noexcept
    {
        const std::string live = current_controlled_character_name();
        if (live.empty())
            return true;
        auto &pm = PresetManager::instance();
        if (live == pm.active_character())
            return true;

        // Defer when load_detect_thread_fn has a pending swap in
        // flight. Convergence is bounded by the settle window
        // (k_charSwapSettleMs); the caller re-arms via a 200ms
        // schedule_transmog_ms() until then.
        if (s_charSwapPending.load(std::memory_order_acquire))
            return false;

        pm.set_active_character(live);
        // Release a stale dropdown pin when the new controlled
        // character is a third protagonist (not the previously
        // controlled, not the pinned). set_active_character only
        // auto-clears the pin when the new controlled IS the pinned
        // char; the third-character case (user pins Damiane while
        // controlling Kliff, then swaps to Oongka in-game) used to
        // keep the pin engaged, so apply_to_state below would
        // populate slot_mappings from Damiane's preset and the next
        // apply would land Damiane's outfit on Oongka. Treat the
        // dropdown selection as transient across real controlled-
        // character swaps: in-game swap releases the pin so editing
        // follows the new body.
        if (pm.editing_pinned() && pm.editing_character() != live)
        {
            pm.clear_editing_pin();
        }
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
        return true;
    }

    // --- Multi-character auto-apply (world-generation triggered) ---
    //
    // Apply one character's preset against an explicit equip-slot a1,
    // bypassing the controlled-char-only path that run_debounced_apply
    // normally takes. Mutates the same global axes (active_character,
    // slot_mappings, last_applied_*) that the single-char path does,
    // so the caller must invoke it from the apply worker thread only
    // (no concurrent writer). Logs failures at debug level since idle
    // (non-controlled) protagonists have engine-zeroed component
    // fields that cause expected chain faults.
    // Returns true if the apply ran (regardless of outcome), false
    // if the body was not yet ready and the caller should re-arm the
    // multi-apply pending flag to retry later.
    [[nodiscard]] static bool apply_for_one_char(
        const std::string &name,
        std::uintptr_t   ccoia) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        if (name.empty() || ccoia == 0)
            return true; // nothing to do; caller should not retry

        const auto equipSlot = CDCore::equip_slot_for_ccoia(ccoia);
        if (equipSlot == 0)
        {
            logger.debug(
                "[multi-apply] {} (ccoia=0x{:X}): equip-slot walk "
                "failed -- skipping",
                name, static_cast<std::uint64_t>(ccoia));
            return true; // CCOIA likely invalid; do not retry
        }

        // Gate on RealPartTearDown::is_actor_apply_ready. Idle /
        // freshly-summoned bodies have partially-initialised mesh
        // containers: tear_down's SEH wrapper catches per-slot
        // faults, but the post-apply pipeline downstream of the
        // first carrier write reads from the same container and
        // throws on the unwired entries. Symptom: ourWrittenCount
        // increases to 1 then apply_all_transmog raises out of the
        // outer __try, leaving the body half-applied. The existing
        // load-detect path (transmog_worker.cpp ~1357) gates the
        // controlled-char apply on this same predicate, so reusing
        // it here keeps the policy consistent.
        if (!RealPartTearDown::is_actor_apply_ready(
                reinterpret_cast<void *>(equipSlot)))
        {
            logger.debug(
                "[multi-apply] {} a1=0x{:X}: body not yet ready "
                "(container chain incomplete) -- will retry",
                name, static_cast<std::uint64_t>(equipSlot));
            return false; // caller should re-arm
        }

        // Swap PresetManager onto this character's axis. Mirrors the
        // sync_active_char_to_live() body so slot_mappings reflect the
        // target preset before apply_all_transmog reads them.
        auto &pm = PresetManager::instance();
        pm.set_active_character(name);
        for (auto &m : slot_mappings())
        {
            m.active = false;
            m.targetItemId = 0;
        }
        pm.apply_to_state();

        // Hydrate the globals from this character's per-body snapshot.
        // On cold-load and after save-load wipes the bucket is all-
        // zero, so the first apply is equivalent to the old
        // fill(0)-style wipe. Mid-session re-applies (e.g., roster-
        // grew on follower summon) preserve the prior installed-state
        // so Phase A teardown still functions across iterations.
        const auto idx = char_idx_for_preset_name(name);
        rehydrate_applied_state_for_char(idx);

        bool faulted = false;
        __try
        {
            apply_all_transmog(static_cast<__int64>(equipSlot));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faulted = true;
        }
        if (faulted)
        {
            // SEH during apply_all_transmog -- the carrier byte
            // patches may have landed but the mesh-rebuild path
            // hit a torn deref on a freshly-summoned body. Report
            // as deferred so the bounded retry catches the next
            // attempt once the body has fully settled.
            logger.debug(
                "[multi-apply] {} a1=0x{:X}: exception during "
                "apply_all_transmog -- deferring retry",
                name, static_cast<std::uint64_t>(equipSlot));
            return false;
        }
        capture_applied_state_for_char(idx);
        logger.debug(
            "[multi-apply] {} a1=0x{:X}",
            name, static_cast<std::uint64_t>(equipSlot));
        return true;
    }

    // Iterate the 1-3 live player CCOIAs from CDCore's static-chain
    // snapshot and apply each character's preset against its own
    // equip-slot. Order: idle protagonists first, currently-controlled
    // last, so PresetManager ends bound to the controlled axis (which
    // the editing UI and subsequent single-char applies key on).
    // Bounded retry counter for partial-chain re-arms. The
    // world_generation() bump fires when the engine reallocates
    // Kliff's CCOIA, but the ChildContainer / actor-list chain
    // used to enumerate Damiane and Oongka is wired separately
    // and lags by a few hundred ms during cold-load. Our first
    // debounced attempt can therefore observe Kliff present but
    // the actor list still empty (snapshot_body_cache returns
    // only Kliff), and on a save genuinely loaded between save
    // points Kliff itself may transiently re-null between the
    // generation bump and the worker firing. In both cases we
    // re-arm the schedule for another attempt; this counter caps
    // the loop to ~30 s of retries (k_multiCharRetryMs *
    // k_multiCharMaxRetries) so a save that genuinely never
    // publishes Damiane or Oongka cannot pin the apply worker
    // forever.
    static std::atomic<int> s_multiCharRetryCount{0};
    static constexpr int          k_multiCharMaxRetries = 30;
    static constexpr std::uint64_t k_multiCharRetryMs   = 1000;

    // Per-world-generation applied-CCOIA tracking. Cold-load and
    // save-load bump world_generation(), at which point the tracking
    // resets and every visible player gets re-applied. In-session
    // snapshot-grew triggers (a follower newly summoned mid-session)
    // only apply for CCOIAs not yet in the applied set -- this
    // avoids re-running apply_all_transmog on the controlled char
    // (who was already applied at cold-load) just because the user
    // summoned a companion. Without this gate every roster-grew
    // trigger would re-flip PresetManager's active_character between
    // each char and incur a full tear-down + carrier-write pass on
    // every already-applied body.
    static std::uint64_t s_lastAppliedWorldGen = 0;
    static std::array<std::uintptr_t, 3> s_appliedCcoias{};
    static std::size_t s_appliedCcoiaCount = 0;

    [[nodiscard]] static bool is_ccoia_already_applied(
        std::uintptr_t ccoia) noexcept
    {
        if (ccoia == 0)
            return false;
        for (std::size_t i = 0; i < s_appliedCcoiaCount; ++i)
            if (s_appliedCcoias[i] == ccoia)
                return true;
        return false;
    }

    static void mark_ccoia_applied(std::uintptr_t ccoia) noexcept
    {
        // The array is sized to the max protagonist count (3), and
        // do_multi_char_apply iterates a 3-slot snapshot, so the
        // count can never exceed s_appliedCcoias.size(). A bounds
        // check would be unreachable; we only guard against the
        // null sentinel.
        if (ccoia == 0)
            return;
        s_appliedCcoias[s_appliedCcoiaCount++] = ccoia;
    }

    // Move the PresetManager axis back to the controlled character
    // WITHOUT running apply_all_transmog. Used after the idle pass
    // when the controlled char was already applied earlier (so we
    // need to restore the editing/UI axis but not redo the work).
    //
    // The applied-state globals MUST be rehydrated from the controlled
    // char's per-body snapshot, not wiped. The prior multi-apply pass
    // captured the controlled char's applied ids into its bucket, and
    // the UI's pending-changes diff compares slot_mappings (just
    // reloaded from the preset) against last_applied_ids. Zeroing here
    // would leave staged != lastIds for every populated slot, surfacing
    // a stale "[PENDING -- click Apply All]" badge for transmog that
    // is in fact already on the body.
    static void rebind_preset_to_controlled(
        const std::string &controlledName) noexcept
    {
        if (controlledName.empty())
            return;
        auto &pm = PresetManager::instance();
        if (pm.active_character() == controlledName)
            return;
        pm.set_active_character(controlledName);
        for (auto &m : slot_mappings())
        {
            m.active = false;
            m.targetItemId = 0;
        }
        pm.apply_to_state();
        const auto idx = char_idx_for_preset_name(controlledName);
        rehydrate_applied_state_for_char(idx);
    }

    static void do_multi_char_apply() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        // Gate on item-catalog readiness. Cold-load completes the
        // CCOIA chain (~500 ms after world_generation bumps) well
        // before the iteminfo background scan finishes the catalog
        // (~3 s on cold-load). Running apply_all_transmog while the
        // catalog is still empty produces slot_mappings with
        // targetItemId=0 across the board, so `apply_all_transmog`
        // writes nothing -- silently dropping every protagonist's
        // saved preset. Hold the schedule here without consuming the
        // retry budget so a slow catalog build does not exhaust the
        // attempt cap.
        if (!ItemNameTable::instance().ready())
        {
            logger.info(
                "[multi-apply] catalog not ready -- re-arming "
                "(retry budget preserved)");
            s_multiCharApplyPending.store(
                true, std::memory_order_release);
            schedule_transmog_ms(k_multiCharRetryMs);
            return;
        }

        // Diagnostic dump of the raw actor list. Logged at trace so a
        // default-level config is silent; the structured `summary` is
        // ALSO consumed by the retry oracle below (summary.actorList
        // distinguishes "chain not yet wired through to the actor
        // list" from "chain reached the list, save only has Kliff").
        CDCore::ActorListDebugSummary summary{};
        {
            std::array<CDCore::ActorListDebugEntry, 24> rawEntries{};
            summary = CDCore::debug_enumerate_actor_list(
                rawEntries.data(), rawEntries.size());
            logger.trace(
                "[multi-apply-diag] chain mgr=0x{:X} ua=0x{:X} "
                "sub=0x{:X} kliff=0x{:X} ctrl=0x{:X} vec=0x{:X} "
                "child=0x{:X} list=0x{:X} rawEntries={}",
                static_cast<std::uint64_t>(summary.mgr),
                static_cast<std::uint64_t>(summary.userActor),
                static_cast<std::uint64_t>(summary.subMgr),
                static_cast<std::uint64_t>(summary.kliffCcoia),
                static_cast<std::uint64_t>(summary.controlled),
                static_cast<std::uint64_t>(summary.vecData),
                static_cast<std::uint64_t>(summary.childContainer),
                static_cast<std::uint64_t>(summary.actorList),
                summary.rawEntries);
            for (std::size_t i = 0; i < summary.rawEntries; ++i)
            {
                const auto &e = rawEntries[i];
                logger.trace(
                    "[multi-apply-diag]   [{:>2}] ccoia=0x{:X} "
                    "flag=0x{:016X} +0x60=0x{:08X} hi=0x{:02X} "
                    "lo=0x{:02X}",
                    i, static_cast<std::uint64_t>(e.ccoia),
                    e.flag, e.identity,
                    static_cast<unsigned>((e.identity >> 24) & 0xFF),
                    static_cast<unsigned>(e.identity & 0xFF));
            }
        }

        std::array<CDCore::BodyCacheEntry, 3> entries{};
        const auto n =
            CDCore::snapshot_body_cache(entries.data(), entries.size());

        // Two partial-chain races to retry past:
        //   (1) sub-manager exists but its +0x30 (Kliff) / +0x38
        //       (controlled) slots are still NULL -- snapshot returns 0.
        //   (2) Kliff/controlled wired but ClientUserActor+0x90 (vec)
        //       / ChildContainer / actor-list chain hasn't been
        //       populated yet -- snapshot returns 1 (Kliff via sub-
        //       manager+0x30), but Damiane/Oongka cannot be found.
        //
        // For case (2) we cannot tell apart "actor list not yet
        // wired" from "save genuinely only has Kliff loaded" without
        // another signal, so we retry up to the cap and let
        // k_multiCharMaxRetries time out cheaply for Kliff-only
        // saves.
        const bool chainIncomplete =
            (n == 0) ||
            (summary.actorList < 0x10000);
        if (chainIncomplete)
        {
            const auto attempt = s_multiCharRetryCount.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            if (attempt <= k_multiCharMaxRetries)
            {
                logger.info(
                    "[multi-apply] partial chain "
                    "(snapshot={}, actorList=0x{:X}); retrying in "
                    "{} ms (attempt {}/{})",
                    n, static_cast<std::uint64_t>(summary.actorList),
                    k_multiCharRetryMs, attempt,
                    k_multiCharMaxRetries);
                s_multiCharApplyPending.store(
                    true, std::memory_order_release);
                schedule_transmog_ms(k_multiCharRetryMs);
                return;
            }
            logger.warning(
                "[multi-apply] gave up after {} attempts -- "
                "chain never finished wiring (snapshot={}, "
                "actorList=0x{:X}); applying what we have",
                k_multiCharMaxRetries, n,
                static_cast<std::uint64_t>(summary.actorList));
            s_multiCharRetryCount.store(
                0, std::memory_order_release);
            // Fall through and apply whatever we have (Kliff only,
            // typically) so we at least don't drop the controlled
            // char's transmog.
        }
        else
        {
            // Full chain reached -- reset retry counter so the next
            // world bump (save-load) starts fresh.
            s_multiCharRetryCount.store(0, std::memory_order_release);
        }
        if (n == 0)
            return;
        // Diagnostic: dump filtered snapshot.
        for (std::size_t i = 0; i < n; ++i)
        {
            logger.trace(
                "[multi-apply-diag] snapshot[{}] ccoia=0x{:X} "
                "charIdx={}",
                i, static_cast<std::uint64_t>(entries[i].body),
                entries[i].charIdx);
        }

        const auto controlledCcoia = CDCore::current_controlled_ccoia();

        // Reset the applied-CCOIA set whenever the world generation
        // changes (cold-load / save-load) so the next pass re-applies
        // every player. On snapshot-grew triggers within the same
        // generation we preserve the set so already-applied chars
        // are skipped.
        const auto curWorldGen = CDCore::world_generation();
        if (curWorldGen != s_lastAppliedWorldGen)
        {
            s_appliedCcoiaCount = 0;
            s_lastAppliedWorldGen = curWorldGen;
        }

        // First pass: idle characters. Skip any CCOIA already applied
        // since the last world-gen bump.
        std::size_t applied = 0;
        std::size_t deferred = 0;
        std::size_t skipped = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (entries[i].body == controlledCcoia)
                continue;
            if (is_ccoia_already_applied(entries[i].body))
            {
                ++skipped;
                continue;
            }
            const auto chEnum =
                static_cast<CDCore::ControlledCharacter>(entries[i].charIdx);
            const auto name =
                std::string(CDCore::controlled_character_name(chEnum));
            if (apply_for_one_char(name, entries[i].body))
            {
                ++applied;
                mark_ccoia_applied(entries[i].body);
            }
            else
            {
                ++deferred;
            }
        }

        // Second pass: controlled character. Two cases:
        //   - First multi-apply of this world generation: controlled
        //     hasn't been applied yet, so apply now and mark.
        //   - Subsequent re-fire (e.g., snapshot grew because of a
        //     summon): controlled was already applied at cold-load.
        //     Skip the apply entirely; just rebind the PresetManager
        //     axis back to controlled so the editing UI / next
        //     hook-driven apply observe the correct character.
        std::string controlledName;
        if (controlledCcoia != 0)
        {
            const auto controlledIdx =
                CDCore::current_controlled_character_idx();
            if (controlledIdx >= 1 && controlledIdx <= 3)
            {
                controlledName = std::string(
                    CDCore::current_controlled_character_name());
                if (!is_ccoia_already_applied(controlledCcoia))
                {
                    if (apply_for_one_char(controlledName, controlledCcoia))
                    {
                        ++applied;
                        mark_ccoia_applied(controlledCcoia);
                    }
                    else
                    {
                        ++deferred;
                    }
                }
                else
                {
                    ++skipped;
                    rebind_preset_to_controlled(controlledName);
                }
            }
        }

        logger.info(
            "[multi-apply] world-entry auto-apply complete: "
            "applied {} of {} player CCOIAs "
            "(deferred={} skipped={})",
            applied, n, deferred, skipped);

        // If any char's body wasn't ready, re-arm with a bounded
        // retry. Freshly-summoned companions take ~1-3 seconds to
        // wire up their mesh container; re-firing the apply on a
        // 1 s cadence catches them once is_actor_apply_ready
        // returns true. Cap at k_multiCharMaxRetries so a body
        // that never becomes ready cannot pin the worker forever.
        if (deferred > 0)
        {
            const auto attempt = s_multiCharRetryCount.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            if (attempt <= k_multiCharMaxRetries)
            {
                logger.info(
                    "[multi-apply] {} char(s) not yet ready; "
                    "re-arming in {} ms (attempt {}/{})",
                    deferred, k_multiCharRetryMs, attempt,
                    k_multiCharMaxRetries);
                s_multiCharApplyPending.store(
                    true, std::memory_order_release);
                schedule_transmog_ms(k_multiCharRetryMs);
            }
            else
            {
                logger.warning(
                    "[multi-apply] {} char(s) never became ready "
                    "after {} attempts -- giving up until next "
                    "world-gen / roster-grew event",
                    deferred, k_multiCharMaxRetries);
                s_multiCharRetryCount.store(
                    0, std::memory_order_release);
            }
        }
        else
        {
            // Everyone applied -- reset the counter so the next
            // world transition starts with a fresh budget.
            s_multiCharRetryCount.store(0, std::memory_order_release);
        }
    }

    // One apply/clear pass. Pulled out of the worker body so the SEH
    // frame does not share scope with the condition-variable unique_lock
    // (MSVC C2712 forbids __try with objects requiring unwinding).
    static void run_debounced_apply() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        // Multi-character world-entry auto-apply takes priority over
        // any pending single-char clear/apply. It iterates every live
        // protagonist CCOIA from CDCore::snapshot_body_cache(),
        // applying each character's preset against its own equip-slot,
        // and ends with PresetManager bound to the controlled char so
        // the rest of the pipeline (UI editing, hook-driven applies)
        // observes the correct axis. Early-return skips the controlled-
        // only path below because do_multi_char_apply() already applied
        // the controlled character last.
        if (s_multiCharApplyPending.exchange(
                false, std::memory_order_acq_rel))
        {
            do_multi_char_apply();
            s_lastApplyOk.store(true, std::memory_order_release);
            return;
        }

        // Consume the clear flag before resolving a1 so a racing
        // manual_clear call doesn't get lost.
        const bool do_clear =
            clear_pending().exchange(false, std::memory_order_acq_rel);

        // Targeted-apply redirect (overlay-UI initiated). When the user
        // has the dropdown pinned to a non-controlled character and
        // `flag_apply_to_editing` is on, the overlay entry points stash
        // that character's 1-based idx here. We re-resolve the CCOIA
        // from the live snapshot at apply time (the body may have been
        // dismissed between schedule and run) and apply the editing
        // character's preset to it. Engine-triggered hooks never set
        // this idx, so VEC / BatchEquip events continue to land on the
        // controlled body via the default path below.
        const auto targetedIdx = s_targetedApplyCharIdx.exchange(
            0, std::memory_order_acq_rel);
        if (targetedIdx >= 1 && targetedIdx <= 3)
        {
            std::array<CDCore::BodyCacheEntry, 3> entries{};
            const auto n = CDCore::snapshot_body_cache(
                entries.data(), entries.size());
            std::uintptr_t targetCcoia = 0;
            for (std::size_t i = 0; i < n; ++i)
            {
                if (entries[i].charIdx == targetedIdx)
                {
                    targetCcoia = entries[i].body;
                    break;
                }
            }
            if (targetCcoia == 0)
            {
                // Editing character is no longer live (follower
                // dismissed between schedule and run, or never was
                // loaded). The user opted out of cross-body apply via
                // the flag, so skip silently rather than render their
                // preset on the controlled body.
                logger.info(
                    "[targeted-apply] editing char idx={} not in live "
                    "snapshot -- skipping apply",
                    targetedIdx);
                s_lastApplyOk.store(true, std::memory_order_release);
                return;
            }
            const auto targetA1 = CDCore::equip_slot_for_ccoia(targetCcoia);
            if (targetA1 == 0)
            {
                logger.info(
                    "[targeted-apply] editing char idx={} ccoia=0x{:X} "
                    "has no equip-slot yet (body still wiring) -- "
                    "re-arming",
                    targetedIdx,
                    static_cast<std::uint64_t>(targetCcoia));
                s_targetedApplyCharIdx.store(
                    targetedIdx, std::memory_order_release);
                schedule_transmog_ms(200);
                return;
            }

            // Per-body state hydrate. Loads this character's
            // last-applied snapshot into the globals so Phase A's
            // teardown decisions ("for slots inactive in the new
            // preset but active in the previous, tear down the
            // installed fake") are made against the items currently
            // on THIS body, not whichever body was applied last.
            // Without this, a preset swap on a non-controlled char
            // would either leak old items (no teardown) or fault
            // (teardown against the wrong body).
            rehydrate_applied_state_for_char(targetedIdx);

            // Consume the single-slot index the same way the default
            // path does. The color-override reinit ("Reload" button)
            // schedules via manual_apply_slot so the worker reaches
            // `apply_single_slot_transmog`, which has the reinit-
            // teardown branch that suppresses the real-item restore
            // during the m.active=false intermediate state. Routing
            // unconditionally through apply_all_transmog (as this
            // path previously did) walks the full pipeline and
            // re-renders the real armor for one frame, causing a
            // visible flash on cross-body reload. Mirror the default
            // path's dispatch so single-slot, all-slot, and clear
            // operations all behave the same regardless of which
            // body they target.
            const auto slotIdx =
                pending_slot_index().exchange(k_slotCount,
                                              std::memory_order_acq_rel);

            bool faulted = false;
            __try
            {
                if (do_clear)
                    clear_all_transmog(static_cast<__int64>(targetA1));
                else if (slotIdx < k_slotCount)
                    apply_single_slot_transmog(
                        static_cast<__int64>(targetA1), slotIdx);
                else
                    apply_all_transmog(static_cast<__int64>(targetA1));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                faulted = true;
            }
            if (faulted)
            {
                s_lastApplyOk.store(false, std::memory_order_release);
                logger.debug(
                    "[targeted-apply] exception during {} on idx={} "
                    "a1=0x{:X}",
                    do_clear ? "clear" : "apply", targetedIdx,
                    static_cast<std::uint64_t>(targetA1));
            }
            else
            {
                // Capture the post-apply globals back into this
                // character's bucket so the next apply sees the
                // up-to-date installed state.
                capture_applied_state_for_char(targetedIdx);
                s_lastApplyOk.store(true, std::memory_order_release);
                logger.debug(
                    "[targeted-apply] {} idx={} slotIdx={} a1=0x{:X}",
                    do_clear      ? "cleared"
                    : slotIdx < k_slotCount ? "applied-slot"
                                            : "applied", targetedIdx,
                    slotIdx, static_cast<std::uint64_t>(targetA1));
            }
            return;
        }

        // Prefer the hook-captured a1 from player_a1() -- it identifies
        // the character whose equip event triggered this apply. Fall
        // back to resolve_player_component() only when player_a1 is
        // empty or unreadable (post-reload / loading screen). The
        // SEH-guarded actor deref below catches a stale wrapper after
        // world reload by dropping the apply.
        //
        // Apply ALWAYS targets the controlled character's body.
        // The helper below syncs PresetManager's controlled axis +
        // slot_mappings to the live character. It lives outside this
        // __try-containing function so std::string destructors do
        // not trip MSVC C2712. When the user has the overlay
        // dropdown pinned to a different editing character, the
        // controlled body still drives carriers and the live source
        // wrappers; the editing character only supplies preset
        // itemIds (cross-body apply).
        //
        // The helper returns false while load-detect has an unfired
        // char swap parked in its settle window. In that case the
        // engine has not yet rotated user+0xD8 to the new body, so
        // applying here would paint the incoming preset onto the
        // previous body. Re-arm the debounce; the load-detect commit
        // will flip the controlled axis once the candidate settles.
        if (!sync_active_char_to_live())
        {
            schedule_transmog_ms(200);
            return;
        }
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

        // Hydrate the globals with the controlled character's per-body
        // snapshot. The default path applies on the controlled body
        // (whether the hook fired naturally or the user invoked manual_-
        // apply without the targeted-redirect flag), so Phase A needs
        // the controlled char's installed-state to make correct
        // teardown decisions when the user swaps presets back-and-
        // forth across protagonists.
        const auto controlledIdx =
            CDCore::current_controlled_character_idx();
        rehydrate_applied_state_for_char(controlledIdx);

        bool faulted = false;
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
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faulted = true;
        }
        if (faulted)
        {
            s_lastApplyOk.store(false, std::memory_order_release);
            // Expected during load-retry window (game visual state not
            // ready yet). Logged at debug to avoid false-alarm noise.
            logger.debug("Transmog {} exception (debounced)",
                         do_clear ? "clear" : "apply");
        }
        else
        {
            capture_applied_state_for_char(controlledIdx);
            s_lastApplyOk.store(true, std::memory_order_release);
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

    void set_targeted_apply_char_idx(std::uint32_t charIdx) noexcept
    {
        s_targetedApplyCharIdx.store(
            charIdx, std::memory_order_release);
    }

    std::uint32_t pending_targeted_apply_char_idx() noexcept
    {
        return s_targetedApplyCharIdx.load(std::memory_order_acquire);
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

    /** @brief SEH-isolated read of `user+0xD8` (the rotating CLIENT
     *         body pointer that EH watches as its save-load signal).
     *  @details Mirrors EH/player_detection.cpp:226 so LT can detect
     *           the same X->0->Y / atomic-swap save-load transitions
     *           EH does and clear preset_manager.active_character()
     *           on the wipe paths. Returns 0 on any fault, on an
     *           unresolved chain, or when the controlled body has
     *           been published as null (engine mid-transition). */
    static std::uintptr_t read_controlled_actor_ptr_seh() noexcept
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
            // user+0xD8 is the CLIENT body pointer (rotates per
            // radial swap or save-load arena allocation). Unlike
            // the other chain reads above, we DO NOT reject 0 here
            // because an X->0 transition is the save-load signal
            // we want to detect.
            auto controlled =
                *reinterpret_cast<uintptr_t *>(user + 0xD8);
            return controlled;
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

        // Save-load detection state. Tracks the CLIENT body pointer
        // (user+0xD8) across ticks. An X->0 transition latches
        // pendingReloadInvalidation; the following 0->Y (with the
        // flag set) OR a non-zero atomic X->Y (disambiguated below
        // via CDCore::world_generation()) triggers the wipe.
        std::uintptr_t prevControlledActor = 0;
        bool pendingReloadInvalidation = false;
        // CDCore world-generation tracking: bumps when the engine
        // reallocates Kliff's CCOIA (cold-load or save-load).
        // Drives two consumers in this loop:
        //   1. atomic-X->Y disambiguation in the controlled-actor
        //      block below (in-session radial swap leaves Kliff's
        //      CCOIA pointer unchanged, so the generation stays
        //      flat);
        //   2. multi-character world-entry auto-apply at the top of
        //      every loop iteration -- on any bump the load thread
        //      arms s_multiCharApplyPending so the apply worker
        //      iterates snapshot_body_cache() and stamps each
        //      protagonist's preset against its own equip-slot.
        //
        // Initialised to 0 so the very first observation reads as a
        // bump and fires the multi-char auto-apply without needing
        // a save-load to bootstrap.
        std::uint64_t prevWorldGen = 0;

        while (!shutdown_requested().load(std::memory_order_relaxed))
        {
            sleep_interruptible(1000);
            if (shutdown_requested().load(std::memory_order_relaxed))
                return 0;

            // --- Multi-character auto-apply triggers ---
            //
            // Two independent signals re-arm the multi-apply path:
            //
            //   (1) CDCore::world_generation() bumps when the engine
            //       reallocates Kliff's CCOIA (cold-load or save-
            //       load). Initialised to 0 so the very first
            //       iteration after thread start fires (cold-load /
            //       LT-loaded-into-live-world).
            //
            //   (2) Player snapshot grew. Kliff's CCOIA is stable
            //       across mid-session "summons" (e.g., user
            //       triggers a UI action that spawns Damiane or
            //       Oongka), so world_generation alone misses these.
            //       We poll snapshot_body_cache each tick; when it
            //       returns more player CCOIAs than before, re-arm.
            //       Guard with prevSnapshotCount > 0 so a fresh world
            //       (0 -> N) does not double-fire alongside the world-
            //       gen path.
            //
            // prevWorldGen is updated by the controlled-actor block
            // below (which also reads curWorldGen for its atomic-X->Y
            // disambiguation), so we do not write it here.
            //
            // The snapshot-count sentinel uses static_cast<size_t>(-1)
            // to mean "uninitialised" (never observed). This is
            // semantically distinct from "observed 0", which the
            // previous `> 0` guard conflated. The world-gen path
            // resets it to uninitialised so the first post-bump
            // iteration just records the snapshot count without
            // re-firing (world-gen already armed the apply).
            static constexpr std::size_t kSnapshotCountUninit =
                static_cast<std::size_t>(-1);
            static std::size_t s_prevSnapshotCount = kSnapshotCountUninit;
            {
                const auto curWorldGen = CDCore::world_generation();
                if (curWorldGen != prevWorldGen)
                {
                    logger.info(
                        "World generation {} -> {}; arming multi-char "
                        "auto-apply (500 ms debounce)",
                        prevWorldGen, curWorldGen);
                    s_multiCharApplyPending.store(
                        true, std::memory_order_release);
                    // Tight first-shot: apply_for_one_char treats a
                    // mesh-walk fault as a deferred retry (re-arms
                    // s_multiCharApplyPending and returns false) so
                    // the bounded retry below picks it up if the
                    // body isn't fully wired yet. Net cost of an
                    // early miss is one extra retry tick; net gain
                    // when the body IS ready is 1.5s faster apply.
                    schedule_transmog_ms(500);
                    s_prevSnapshotCount = kSnapshotCountUninit;
                }
                else
                {
                    std::array<CDCore::BodyCacheEntry, 3> snap{};
                    const auto n = CDCore::snapshot_body_cache(
                        snap.data(), snap.size());
                    if (s_prevSnapshotCount != kSnapshotCountUninit &&
                        n > s_prevSnapshotCount)
                    {
                        logger.info(
                            "Player roster grew {} -> {}; arming "
                            "multi-char auto-apply (500 ms debounce)",
                            s_prevSnapshotCount, n);
                        s_multiCharApplyPending.store(
                            true, std::memory_order_release);
                        // Match the world-gen debounce; the deferred-
                        // retry path covers the "newly-summoned but
                        // not-yet-wired" window.
                        schedule_transmog_ms(500);
                    }
                    s_prevSnapshotCount = n;
                }
            }

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
                        // Same rationale as wipe_lt_state in the
                        // controlled-actor branch below: every fake
                        // installed against the previous arena is
                        // gone, so the per-body trackers must reset
                        // before the next apply hydrates them.
                        reset_all_applied_state();
                        PrefabWrapperSwap::reset_per_char_state();
                        // Re-populate the body-mesh prefab catalog. Save
                        // loads can rotate the AppearanceTableLoader
                        // registry's resident wrapper set as zone-/
                        // archetype-specific assets stream in/out, and
                        // the picker dropdown otherwise stays pinned to
                        // the boot snapshot until the user clicks
                        // "Refresh Catalog" manually. Idempotent and
                        // cheap (~5ms StringInfo walk + ~10ms registry
                        // enum); fires once per save-load tick.
                        PrefabWrapperSwap::populate_slot_catalogs();
                    }
                    prevUser = curUser;
                }
            }

            // --- Save-load detected (controlled-actor signal) ---
            //
            // Mirrors EquipHide/player_detection.cpp. Polls
            // user+0xD8 (the CLIENT controlled body pointer) and
            // disambiguates three transitions:
            //   1. X->0 : engine published null -> latch deferred wipe
            //   2. 0->Y with flag set : world live again -> wipe
            //   3. X->Y atomic : CDCore::world_generation() bump = the
            //                    engine reallocated Kliff's CCOIA
            //                    (save-load); unchanged generation =
            //                    in-session radial swap.
            //
            // On every wipe path we ALSO clear LT-local state that
            // would otherwise route through the previously-active
            // character's preset (preset_manager.active_character()
            // and the prevCharName/pendingCharName settle window).
            // This is the "On save load detected, should clear current
            // char of LT" requirement: empty active_character()
            // resolves to no preset -> engine vanilla items show
            // until the next chain walk observes the correct identity.
            {
                const auto controlledActor =
                    read_controlled_actor_ptr_seh();
                const auto curWorldGen = CDCore::world_generation();
                if (controlledActor != prevControlledActor)
                {
                    auto wipe_lt_state = [&]()
                    {
                        CDCore::invalidate_controlled_character();
                        PresetManager::instance().set_active_character("");
                        prevCharName.clear();
                        pendingCharName.clear();
                        // The engine reallocated every actor body during
                        // this transition, so all fakes the previous
                        // session installed are gone. Drop both the
                        // globals and the per-character snapshot buffers
                        // so the next apply hydrates from a clean slate
                        // instead of trying to tear down items that no
                        // longer exist on the new arena.
                        reset_all_applied_state();
                        PrefabWrapperSwap::reset_per_char_state();
                    };

                    if (controlledActor == 0 && prevControlledActor != 0)
                    {
                        logger.info(
                            "Save-load detected: controlled actor "
                            "(0x{:X} -> 0x0); deferring full cache wipe "
                            "until new world is live",
                            static_cast<uint64_t>(prevControlledActor));
                        pendingReloadInvalidation = true;
                    }
                    else if (controlledActor != 0 &&
                             prevControlledActor == 0 &&
                             pendingReloadInvalidation)
                    {
                        logger.info(
                            "Save-load complete: new controlled actor "
                            "0x{:X} -- clearing LT preset_manager "
                            "active character + swap-scope state",
                            static_cast<uint64_t>(controlledActor));
                        wipe_lt_state();
                        pendingReloadInvalidation = false;
                    }
                    else if (prevControlledActor != 0 &&
                             controlledActor != 0)
                    {
                        // Atomic X->Y. World-generation bump = the
                        // engine reallocated Kliff's CCOIA during
                        // save-load. No bump = in-session radial
                        // swap; preserve LT state.
                        const bool atomicSaveLoad =
                            curWorldGen != prevWorldGen;

                        if (atomicSaveLoad)
                        {
                            logger.info(
                                "Save-load detected (atomic swap): "
                                "controlled actor (0x{:X} -> 0x{:X}); "
                                "world_generation {} -> {}; clearing "
                                "LT preset_manager active character + "
                                "swap-scope state",
                                static_cast<uint64_t>(prevControlledActor),
                                static_cast<uint64_t>(controlledActor),
                                prevWorldGen, curWorldGen);
                            wipe_lt_state();
                            pendingReloadInvalidation = false;
                        }
                        // else: normal char swap -- LT's existing
                        // char-swap auto-detect block (below) picks
                        // up the identity change via the live chain
                        // walk and calls set_active_character.
                    }
                    prevControlledActor = controlledActor;
                }
                prevWorldGen = curWorldGen;
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
                    s_charSwapPending.store(false,
                                            std::memory_order_release);
                }
                else if (liveName != pendingCharName)
                {
                    pendingCharName = liveName;
                    pendingFirstSeenTick = GetTickCount64();
                    s_charSwapPending.store(true,
                                            std::memory_order_release);
                }
                else if (GetTickCount64() - pendingFirstSeenTick >=
                         k_charSwapSettleMs)
                {
                    const std::string oldName = prevCharName.empty()
                                                    ? pm.active_character()
                                                    : prevCharName;
                    prevCharName = liveName;
                    pendingCharName.clear();
                    // Clear BEFORE schedule_transmog_ms below so the
                    // scheduled run_debounced_apply observes the
                    // committed flip and does not defer again.
                    s_charSwapPending.store(false,
                                            std::memory_order_release);

                    if (liveName != pm.active_character())
                    {
                        pm.set_active_character(liveName);
                        // An in-game controlled-character change to a
                        // body different from any prior dropdown
                        // "pin" must release that pin: the dropdown
                        // selection is treated as transient across
                        // real controlled-character swaps. Without
                        // this release, the worker would feed the
                        // pinned character's preset into
                        // slot_mappings via apply_to_state below and
                        // schedule_transmog would apply it to the
                        // newly-controlled body (e.g. user pins
                        // Damiane while controlling Kliff, then
                        // swaps to Oongka -- without release,
                        // Damiane's outfit lands on Oongka).
                        // set_active_character already auto-clears
                        // the pin when the new controlled char IS
                        // the pinned char; this handles the third-
                        // character case.
                        if (pm.editing_pinned() &&
                            pm.editing_character() != liveName)
                        {
                            pm.clear_editing_pin();
                        }
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
                // Resolve identity WITHOUT invalidating any CDCore
                // cache. The focus-broadcast resolver stamps Tier-0
                // on every engine focus event, so a stale read here is
                // self-correcting within one tick. Calling
                // invalidate_controlled_character() inline would force
                // an Unknown window on saves whose first broadcast has
                // not arrived yet (Prologue / post-cutscene resume),
                // gating the auto-apply indefinitely.
                const std::string liveChar =
                    current_controlled_character_name();

                // Advance prevComp BEFORE the controlled-char gate so
                // the worker does not re-observe the same change every
                // tick while the world is still loading. prevComp
                // tracks the most recent observed value, not the most
                // recent successfully-applied one.
                logger.info(
                    "Load detect: player component changed "
                    "({:#x} -> {:#x}); controlled = {}",
                    static_cast<uint64_t>(prevComp),
                    static_cast<uint64_t>(comp),
                    liveChar.empty() ? std::string_view{"<unresolved>"}
                                     : std::string_view{liveChar});
                prevComp = comp;

                // Engine has rotated player_component to the new body,
                // so the race window that s_charSwapPending guards is
                // closed regardless of whether the char-swap auto-
                // detect block above has finished its settle window.
                // Without this clear, the retry loop below blocks the
                // load-detect thread for several seconds, the auto-
                // detect block never re-ticks during that span, and
                // sync_active_char_to_live keeps deferring every
                // scheduled apply -- the visible symptom is "scheduled
                // apply (attempt N)" loglines with no apply ever
                // landing after a save-load or radial swap.
                s_charSwapPending.store(false,
                                        std::memory_order_release);

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
