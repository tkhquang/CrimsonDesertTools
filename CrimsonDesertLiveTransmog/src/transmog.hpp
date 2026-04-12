#pragma once

namespace Transmog
{
    bool init();
    void shutdown();

    void manual_apply();
    void manual_clear();
    void capture_outfit();

    /// Capture current real equipment into slot_mappings (from entry table, not transmog state).
    void capture_real_equipment();

    /// Returns true once the WorldSystem chain resolves to a live
    /// player component. Overlay buttons should gate on this to avoid
    /// spamming "player not found" before the first world load.
    bool is_world_ready() noexcept;

} // namespace Transmog
