#pragma once

namespace EquipHide
{
    /** @brief Write vis-byte modifications for all tracked parts across all protagonists. */
    void apply_direct_vis_write() noexcept;

    /**
     * @brief Restore all modified vis bytes to their original values.
     * @details Restores only entries tracked in original_vis_map. Unmodified
     *          entries (including injected zero-bone armor entries) are left as-is.
     */
    void cleanup_vis_bytes() noexcept;

} // namespace EquipHide
