#pragma once

#include <cstddef>

namespace EquipHide
{
    /** @brief Write vis-byte modifications for all tracked parts across all protagonists. */
    void apply_direct_vis_write() noexcept;

    /**
     * @brief Restore all modified vis bytes to their original values.
     * @details Restores only entries tracked in original_vis_map. Unmodified entries (including injected zero-bone
     *          armor entries) are left as-is.
     */
    void cleanup_vis_bytes() noexcept;

    /**
     * @brief Offset of the vis byte within the engine's PartInOut struct.
     * @details Self-heals: decoded once from the resolved EquipVisCheck
     *          instruction (`movzx eax, byte [r12+disp]`) so a future PartInOut re-layout self-corrects, with 0x20 as
     *          the validated nominal. The decision struct EquipVisCheck reads and the IndexedString map entry the
     *          direct-write path mutates are the same PartInOut layout -- the engine fills the decision struct by
     *          copying the map entry field-for-field -- so this single offset is correct for every vis-byte access.
     *          Warmed at hook install; the hot path then only reads the cached value.
     */
    [[nodiscard]] std::size_t vis_byte_offset() noexcept;

} // namespace EquipHide
