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
     * @details Self-heals: decoded once from the displacement of the resolved EquipVisCheck instruction
     *          (`movzx <reg>, byte [<reg>+disp]`) so a PartInOut re-layout self-corrects, with 0x20 as the
     *          validated nominal kept on any miss. Which registers that instruction uses is a compiler choice and
     *          the decode ignores them. The decision struct EquipVisCheck reads and the IndexedString map entry the
     *          direct-write path mutates are the same PartInOut layout -- the engine fills the decision struct by
     *          copying the map entry field-for-field -- so this single offset is correct for every vis-byte access.
     *          Warmed at hook install; the hot path then only reads the cached value.
     */
    [[nodiscard]] std::size_t vis_byte_offset() noexcept;

    /**
     * @name Vis-ctrl to part-visibility-map pointer chain
     * @details `mapBase = *(*(vc + k_visCtrlToCccOffset) + k_cccToDescriptorOffset) + k_descriptorToPartVisMapOffset`.
     *
     *          The direct-write and armor-injection passes both walk it and must agree, so the values live here and
     *          in no other translation unit. A copy in each caller is one offset to miss when the layout shifts.
     *
     *          To re-verify: the decision-struct builder that feeds EquipVisCheck walks the same chain, opening
     *          with `mov rax,[rcx+<ccc>] ; mov rcx,[rax+<desc>] ; ... ; add rcx,<map>` immediately ahead of its map
     *          lookup call. Read the three displacements out of that window rather than trusting these constants.
     *
     *          A wrong descriptor offset does NOT fault. It reads a neighboring field, which is either zero (the
     *          map base then lands on the bare map offset and the plausible-pointer gate rejects it) or a
     *          non-pointer scalar whose low bits still look mapped. It surfaces as the "implausible mapBase" and
     *          "not a valid part-vis map" traces, never as a crash.
     * @{
     */
    inline constexpr std::size_t k_visCtrlToCccOffset = 0x88;
    inline constexpr std::size_t k_cccToDescriptorOffset = 0x220;
    inline constexpr std::size_t k_descriptorToPartVisMapOffset = 0x28;
    /** @} */

} // namespace EquipHide
