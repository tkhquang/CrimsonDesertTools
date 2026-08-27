#include "shared_state.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <mutex>

namespace Transmog
{
    std::string runtime_dir_utf8()
    {
        std::wstring dirW = DMK::Filesystem::get_runtime_directory();
        if (dirW.empty())
            return {};
        const int n =
            WideCharToMultiByte(CP_UTF8, 0, dirW.data(), static_cast<int>(dirW.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
            return {};
        std::string dir(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, dirW.data(), static_cast<int>(dirW.size()), dir.data(), n, nullptr, nullptr);
        if (dir.back() != '\\' && dir.back() != '/')
            dir.push_back('\\');
        return dir;
    }

    static ResolvedAddresses s_resolvedAddrs{};
    static std::array<SlotMapping, k_slotCount> s_slotMappings{};
    static std::array<uint16_t, k_slotCount> s_lastAppliedIds{};

    static std::atomic<bool> s_playerOnly{true};
    static std::atomic<bool> s_enabled{true};
    static std::atomic<bool> s_shutdownRequested{false};
    static std::atomic<bool> s_colorOverride{false};
    static std::atomic<bool> s_helmAudioUnmuffle{true};
    static std::atomic<bool> s_dumpItemPrefabs{false};
    static std::atomic<bool> s_dumpItemCatalog{false};
    static std::atomic<bool> s_applyToEditing{true};

    static SlotPopulatorFn s_slotPopulator = nullptr;
    static PartSlotRefreshFn s_partSlotRefresh = nullptr;
    static SlotTagToHandleFn s_slotTagToHandle = nullptr;
    static ItemToSlotResolveFn s_itemToSlotResolve = nullptr;
    static InitSwapEntryFn s_initSwapEntry = nullptr;

    static std::atomic<bool> s_inTransmog{false};
    static std::atomic<bool> s_suppressVEC{false};
    static std::atomic<__int64> s_playerA1{0};
    static std::atomic<uintptr_t> s_worldSystemPtr{0};
    static std::array<bool, k_slotCount> s_realDamaged{};
    static std::array<std::uint16_t, k_slotCount> s_lastAppliedRealIds{};
    static std::atomic<bool> s_clearPending{false};
    static std::atomic<bool> s_dyeDirty{false};
    static std::atomic<std::size_t> s_pendingSlotIndex{k_slotCount};
    static std::array<std::uint16_t, k_slotCount> s_lastAppliedCarrierIds{};
    static std::array<bool, k_slotCount> s_forceApplyPending{};

    // Per-character buffered snapshots of the four applied-state arrays above. Indexed by (idx-1) where idx is the
    // 1-based CDCore protagonist index (1=Kliff, 2=Damiane, 3=Oongka). The worker hydrates the globals from the
    // relevant slot before each apply and writes the post-apply globals back, so Phase A teardown always sees a
    // per-body truth source.
    static std::array<std::array<std::uint16_t, k_slotCount>, 3> s_lastAppliedIdsPerChar{};
    static std::array<std::array<bool, k_slotCount>, 3> s_realDamagedPerChar{};
    static std::array<std::array<std::uint16_t, k_slotCount>, 3> s_lastAppliedRealIdsPerChar{};
    static std::array<std::array<std::uint16_t, k_slotCount>, 3> s_lastAppliedCarrierIdsPerChar{};

    ResolvedAddresses &resolved_addrs()
    {
        return s_resolvedAddrs;
    }
    std::array<SlotMapping, k_slotCount> &slot_mappings()
    {
        return s_slotMappings;
    }
    std::array<uint16_t, k_slotCount> &last_applied_ids()
    {
        return s_lastAppliedIds;
    }

    std::atomic<bool> &flag_player_only()
    {
        return s_playerOnly;
    }
    std::atomic<bool> &flag_enabled()
    {
        return s_enabled;
    }
    std::atomic<bool> &shutdown_requested()
    {
        return s_shutdownRequested;
    }
    std::atomic<bool> &flag_color_override()
    {
        return s_colorOverride;
    }
    std::atomic<bool> &flag_helm_audio_unmuffle()
    {
        return s_helmAudioUnmuffle;
    }
    std::atomic<bool> &flag_dump_item_prefabs()
    {
        return s_dumpItemPrefabs;
    }
    std::atomic<bool> &flag_dump_item_catalog()
    {
        return s_dumpItemCatalog;
    }
    std::atomic<bool> &flag_apply_to_editing()
    {
        return s_applyToEditing;
    }

    PartSlotRefreshFn &part_slot_refresh_fn()
    {
        return s_partSlotRefresh;
    }

    SlotTagToHandleFn &slot_tag_to_handle_fn()
    {
        return s_slotTagToHandle;
    }

    ItemToSlotResolveFn &item_to_slot_resolve_fn()
    {
        return s_itemToSlotResolve;
    }

    SlotPopulatorFn &slot_populator_fn()
    {
        return s_slotPopulator;
    }
    InitSwapEntryFn &init_swap_entry_fn()
    {
        return s_initSwapEntry;
    }

    std::atomic<bool> &in_transmog()
    {
        return s_inTransmog;
    }
    std::atomic<__int64> &player_a1()
    {
        return s_playerA1;
    }

    std::string current_controlled_character_name() noexcept
    {
        // Delegates to the shared Core resolver (focus-broadcast cache populated by sub_14353BA60's R9 hash, with LKG /
        // structural
        // Kliff fallbacks). Returns an empty string when the resolver has not yet observed a known identity this
        // session.
        const auto name = CDCore::current_controlled_character_name();
        return std::string(name);
    }
    std::atomic<uintptr_t> &world_system_ptr()
    {
        return s_worldSystemPtr;
    }
    std::array<bool, k_slotCount> &real_damaged()
    {
        return s_realDamaged;
    }
    std::array<std::uint16_t, k_slotCount> &last_applied_real_ids()
    {
        return s_lastAppliedRealIds;
    }
    std::array<std::uint16_t, k_slotCount> &last_applied_carrier_ids()
    {
        return s_lastAppliedCarrierIds;
    }
    std::array<bool, k_slotCount> &force_apply_pending()
    {
        return s_forceApplyPending;
    }
    std::atomic<bool> &clear_pending()
    {
        return s_clearPending;
    }
    std::atomic<bool> &dye_dirty()
    {
        return s_dyeDirty;
    }
    std::atomic<std::size_t> &pending_slot_index()
    {
        return s_pendingSlotIndex;
    }

    void rehydrate_applied_state_for_char(std::uint32_t idx) noexcept
    {
        if (idx < 1 || idx > 3)
            return;
        const auto bucket = static_cast<std::size_t>(idx - 1);
        s_lastAppliedIds = s_lastAppliedIdsPerChar[bucket];
        s_realDamaged = s_realDamagedPerChar[bucket];
        s_lastAppliedRealIds = s_lastAppliedRealIdsPerChar[bucket];
        s_lastAppliedCarrierIds = s_lastAppliedCarrierIdsPerChar[bucket];
    }

    void capture_applied_state_for_char(std::uint32_t idx) noexcept
    {
        if (idx < 1 || idx > 3)
            return;
        const auto bucket = static_cast<std::size_t>(idx - 1);
        s_lastAppliedIdsPerChar[bucket] = s_lastAppliedIds;
        s_realDamagedPerChar[bucket] = s_realDamaged;
        s_lastAppliedRealIdsPerChar[bucket] = s_lastAppliedRealIds;
        s_lastAppliedCarrierIdsPerChar[bucket] = s_lastAppliedCarrierIds;
    }

    void reset_applied_state_for_char(std::uint32_t idx) noexcept
    {
        if (idx < 1 || idx > 3)
            return;
        const auto bucket = static_cast<std::size_t>(idx - 1);
        s_lastAppliedIdsPerChar[bucket].fill(0);
        s_realDamagedPerChar[bucket].fill(false);
        s_lastAppliedRealIdsPerChar[bucket].fill(0);
        s_lastAppliedCarrierIdsPerChar[bucket].fill(0);
        // Also wipe the live globals: apply_all_transmog reads these directly (last_applied_ids / real_damaged /
        // last_applied_real_ids / last_applied_carrier_ids), so a stale global would drive the no-change early-out
        // even after the bucket was cleared. rehydrate_applied_state_for_char would normally overwrite the globals from
        // the bucket, but the body-reallocation path skips rehydrate by design and calls this instead.
        s_lastAppliedIds.fill(0);
        s_realDamaged.fill(false);
        s_lastAppliedRealIds.fill(0);
        s_lastAppliedCarrierIds.fill(0);
    }

    // --- Protagonist body-ownership table ---
    //
    // One producer (the load-detect worker) and many readers (engine threads inside the socket-build detour). The
    // producer owns the expensive part, the actor-array walk, so a reader never pays for it.
    //
    // Each row keeps the CCOIA it was derived from alongside the equip-slot address, because the address alone is not
    // a safe key. The engine pools both objects, so between two publishes a body can be freed and its addresses handed
    // to an unrelated actor, and a bare address compare would then report a protagonist index for somebody else's
    // body.
    //
    // A hit therefore confirms two independent things before it trusts the row, because either alone leaves a hole:
    //   - the CCOIA still classifies as the same character, which rules out the pool reissuing the actor. A structural
    //     walk cannot detect that on its own, since the successor object occupies the identical layout;
    //   - the CCOIA still resolves to this equip slot, which rules out the slot being reissued on its own.
    // Both are paid only by the handful of bodies that match an entry, never by the NPCs and creatures that make up
    // the traffic, and together they turn a silent mis-identification into an ordinary miss.
    //
    // A plain mutex rather than a reader/writer lock: the guarded region is a scan of at most three integers, and the
    // detour reaches it on the order of once per second, so shared-reader parallelism would buy nothing that the
    // narrower critical section does not already give.
    namespace
    {
        /// One published protagonist body. Plain data, no invariant beyond what publish_body_owner_table enforces.
        struct BodyOwnerRow
        {
            std::uintptr_t ccoia;
            std::uintptr_t equipSlot;
            std::uint32_t charIdx;
        };

        std::array<BodyOwnerRow, k_bodyOwnerCap> s_bodyOwners{};
        std::size_t s_bodyOwnerCount = 0;
        std::mutex s_bodyOwnerMutex;
    } // namespace

    void publish_body_owner_table(const std::uintptr_t *ccoias, const std::uint32_t *charIdxs, std::size_t n) noexcept
    {
        if (ccoias == nullptr || charIdxs == nullptr)
            return;

        // Resolve before taking the lock. equip_slot_for_ccoia walks engine memory under SEH, and holding a lock
        // across a foreign-memory read would expose every reader to whatever that walk costs on a torn chain.
        std::array<BodyOwnerRow, k_bodyOwnerCap> built{};
        std::size_t written = 0;
        for (std::size_t i = 0; i < n && i < built.size(); ++i)
        {
            const auto slot = CDCore::equip_slot_for_ccoia(ccoias[i]);
            if (slot == 0)
                continue; // component chain not wired yet; the next publish picks this body up
            built[written] = BodyOwnerRow{ccoias[i], slot, charIdxs[i]};
            ++written;
        }

        // An exhausted snapshot leaves written at 0, which publishes an empty table. That is deliberate: holding the
        // previous rows through a teardown is the dangerous direction, because their bodies are freed and their
        // addresses reissued, so a surviving row would name a dead character as the owner.
        std::scoped_lock lk(s_bodyOwnerMutex);
        s_bodyOwners = built;
        s_bodyOwnerCount = written;
    }

    std::uint32_t char_idx_for_equip_slot_uncached(std::uintptr_t a1) noexcept
    {
        if (a1 < 0x10000)
            return 0;
        std::array<CDCore::BodyCacheEntry, 3> entries{};
        const auto n = CDCore::snapshot_body_cache(entries.data(), entries.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            if (CDCore::equip_slot_for_ccoia(entries[i].body) == a1)
                return entries[i].charIdx;
        }
        return 0;
    }

    std::uint32_t char_idx_for_equip_slot(std::uintptr_t a1) noexcept
    {
        if (a1 < 0x10000)
            return 0;

        std::uintptr_t ccoia = 0;
        std::uint32_t charIdx = 0;
        {
            std::scoped_lock lk(s_bodyOwnerMutex);
            for (std::size_t i = 0; i < s_bodyOwnerCount; ++i)
            {
                if (s_bodyOwners[i].equipSlot == a1)
                {
                    ccoia = s_bodyOwners[i].ccoia;
                    charIdx = s_bodyOwners[i].charIdx;
                    break;
                }
            }
        }
        if (ccoia == 0)
            return 0; // every NPC and creature lands here, having paid at most three integer compares

        // Confirmed outside the lock, for the reasons given on the table above.
        if (CDCore::character_idx_for_ccoia(ccoia) != charIdx)
            return 0;
        return CDCore::equip_slot_for_ccoia(ccoia) == a1 ? charIdx : 0;
    }

    std::atomic<std::uint32_t> &slot_mappings_owner() noexcept
    {
        static std::atomic<std::uint32_t> s_owner{0};
        return s_owner;
    }

    void reset_all_applied_state() noexcept
    {
        s_lastAppliedIds.fill(0);
        s_realDamaged.fill(false);
        s_lastAppliedRealIds.fill(0);
        s_lastAppliedCarrierIds.fill(0);
        for (auto &row : s_lastAppliedIdsPerChar)
            row.fill(0);
        for (auto &row : s_realDamagedPerChar)
            row.fill(false);
        for (auto &row : s_lastAppliedRealIdsPerChar)
            row.fill(0);
        for (auto &row : s_lastAppliedCarrierIdsPerChar)
            row.fill(0);
    }

} // namespace Transmog
