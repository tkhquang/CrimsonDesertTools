#include "shared_state.hpp"

namespace Transmog
{
    static ResolvedAddresses s_resolvedAddrs{};
    static std::array<SlotMapping, k_slotCount> s_slotMappings{};
    static std::array<uint16_t, k_slotCount> s_lastAppliedIds{};

    static std::atomic<bool> s_playerOnly{true};
    static std::atomic<bool> s_enabled{true};
    static std::atomic<bool> s_shutdownRequested{false};

    static SlotPopulatorFn s_slotPopulator = nullptr;
    static InitSwapEntryFn s_initSwapEntry = nullptr;

    static std::atomic<bool> s_inTransmog{false};
    static std::atomic<bool> s_suppressVEC{false};
    static std::atomic<__int64> s_playerA1{0};
    static std::atomic<uintptr_t> s_worldSystemPtr{0};
    static std::array<bool, k_slotCount> s_realDamaged{};
    static std::array<std::uint16_t, 5> s_lastAppliedRealIds{};
    static std::atomic<bool> s_clearPending{false};
    static std::atomic<std::size_t> s_pendingSlotIndex{k_slotCount};
    static std::array<std::uint16_t, k_slotCount> s_lastAppliedCarrierIds{};

    ResolvedAddresses &resolved_addrs() { return s_resolvedAddrs; }
    std::array<SlotMapping, k_slotCount> &slot_mappings() { return s_slotMappings; }
    std::array<uint16_t, k_slotCount> &last_applied_ids() { return s_lastAppliedIds; }

    std::atomic<bool> &flag_player_only() { return s_playerOnly; }
    std::atomic<bool> &flag_enabled() { return s_enabled; }
    std::atomic<bool> &shutdown_requested() { return s_shutdownRequested; }

    SlotPopulatorFn &slot_populator_fn() { return s_slotPopulator; }
    InitSwapEntryFn &init_swap_entry_fn() { return s_initSwapEntry; }

    std::atomic<bool> &in_transmog() { return s_inTransmog; }
    std::atomic<bool> &suppress_vec() { return s_suppressVEC; }
    std::atomic<__int64> &player_a1() { return s_playerA1; }
    std::atomic<uintptr_t> &world_system_ptr() { return s_worldSystemPtr; }
    std::array<bool, k_slotCount> &real_damaged() { return s_realDamaged; }
    std::array<std::uint16_t, 5> &last_applied_real_ids() { return s_lastAppliedRealIds; }
    std::array<std::uint16_t, k_slotCount> &last_applied_carrier_ids() { return s_lastAppliedCarrierIds; }
    std::atomic<bool> &clear_pending() { return s_clearPending; }
    std::atomic<std::size_t> &pending_slot_index() { return s_pendingSlotIndex; }

} // namespace Transmog
