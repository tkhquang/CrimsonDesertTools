#include "shared_state.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

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
    std::atomic<bool> &suppress_vec()
    {
        return s_suppressVEC;
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
