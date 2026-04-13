#pragma once

#include <cstddef>

namespace Transmog
{
    bool init();
    void shutdown();

    void manual_apply();

    /// @brief Schedule a single-slot transmog apply via the debounce worker.
    /// @param slotIdx TransmogSlot index (0..4). Only this slot is torn
    ///        down and re-applied; other slots are untouched.
    void manual_apply_slot(std::size_t slotIdx);

    void manual_clear();
    void capture_outfit();

    /// Capture current real equipment into slot_mappings (from entry table, not transmog state).
    void capture_real_equipment();

    /// Returns true once the WorldSystem chain resolves to a live
    /// player component. Overlay buttons should gate on this to avoid
    /// spamming "player not found" before the first world load.
    bool is_world_ready() noexcept;

} // namespace Transmog
