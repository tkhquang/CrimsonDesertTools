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

    /// Read the engine's live dye records for the auth-table entry
    /// whose gameTag maps to `slotIdx` and write them into the active
    /// preset's per-slot dye[] array (replacing whatever was stored).
    /// Sets `dyeSparse=true` and `dye_dirty()=true` on success. Used
    /// by the per-slot "sync from live" button when the user has
    /// applied in-game dye to a fake-bound slot's underlying real
    /// item and wants those bytes saved into the active preset.
    /// Returns true if at least one channel was captured. No-op when
    /// no active preset, slot disabled, or auth-table entry missing.
    bool sync_live_dye_for_slot(std::size_t slotIdx) noexcept;

    /// Returns true once the WorldSystem chain resolves to a live
    /// player component. Overlay buttons should gate on this to avoid
    /// spamming "player not found" before the first world load.
    bool is_world_ready() noexcept;

} // namespace Transmog
