#ifndef EQUIPHIDE_CASCADE_SUPPRESS_HPP
#define EQUIPHIDE_CASCADE_SUPPRESS_HPP

#include <cstdint>

namespace EquipHide
{
    /** @brief VisualEquipChange trampoline signature: the engine entry point that applies one slot change. */
    using VisualEquipChangeFn = __int64(__fastcall *)(__int64, int16_t, int16_t, __int64);

    /**
     * @brief VisualEquipChange detour. Flags a chest-slot equip or unequip so the mid-hook clears the cascade locks.
     * @param bodyComp The body component the engine updates.
     * @param slotId The equipment slot the engine writes.
     * @param itemId The item the engine places in that slot.
     * @param itemData The engine's per-item payload, passed straight through.
     * @return The original slot-update result, or 0 once the hook drops its trampoline.
     */
    __int64 __fastcall on_visual_equip_change(__int64 bodyComp, int16_t slotId, int16_t itemId, __int64 itemData);

    /** @brief Publish the VisualEquipChange trampoline after hook installation. */
    void set_visual_equip_change_trampoline(VisualEquipChangeFn original);

    /** @brief VisualEquipSwap trampoline signature: the engine entry point that swaps one item for another. */
    using VisualEquipSwapFn = __int64(__fastcall *)(__int64 *, __int64 *, __int64 **, __int64 **);

    /**
     * @brief VisualEquipSwap detour. Reads the dispatch list and flags a swap that touches the chest slot.
     * @param a1 Engine dispatch context, passed straight through.
     * @param a2 Engine dispatch context, passed straight through.
     * @param a3 Engine dispatch context, passed straight through.
     * @param a4 The swap-entry list head the detour walks for slot ids.
     * @return The original swap result, or 0 once the hook drops its trampoline.
     */
    __int64 __fastcall on_visual_equip_swap(__int64 *a1, __int64 *a2, __int64 **a3, __int64 **a4);

    /** @brief Publish the VisualEquipSwap trampoline after hook installation, and warm the layout decode. */
    void set_visual_equip_swap_trampoline(VisualEquipSwapFn original);

    /**
     * @brief Consumes the pending visual-equip-change flag.
     * @return true on the first call after either detour flags a chest change, false afterwards.
     */
    [[nodiscard]] bool consume_equip_change() noexcept;

} // namespace EquipHide

#endif // EQUIPHIDE_CASCADE_SUPPRESS_HPP
