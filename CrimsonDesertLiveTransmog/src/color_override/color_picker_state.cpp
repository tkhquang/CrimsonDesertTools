#include "color_picker_state.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace Transmog::ColorOverride::PickerState
{
    namespace
    {
        // One bool per slot. The per-row user RGB lives in
        // SwatchTable::override_row(); this flag is just the coarse
        // "user has anything overridden in this slot" gate.
        std::array<std::atomic<bool>, k_slotCount> g_slotActive{};
    }

    void set_slot_override_active(int slot, bool active) noexcept
    {
        if (slot < 0 || static_cast<std::size_t>(slot) >= k_slotCount)
            return;
        g_slotActive[static_cast<std::size_t>(slot)].store(
            active, std::memory_order_release);
    }

    bool is_slot_override_active(int slot) noexcept
    {
        if (slot < 0 || static_cast<std::size_t>(slot) >= k_slotCount)
            return false;
        return g_slotActive[static_cast<std::size_t>(slot)].load(
            std::memory_order_acquire);
    }
}
