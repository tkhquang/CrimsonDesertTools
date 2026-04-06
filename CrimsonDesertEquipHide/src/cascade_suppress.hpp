#pragma once

#include <cstdint>

namespace EquipHide
{
    // --- VisualEquipChange hook (equipment change detection) ---

    using VisualEquipChangeFn = __int64(__fastcall *)(__int64, int16_t, int16_t, __int64);

    __int64 __fastcall on_visual_equip_change(
        __int64 bodyComp, int16_t slotId, int16_t itemId, __int64 itemData);

    void set_visual_equip_change_trampoline(VisualEquipChangeFn original);

    // --- VisualEquipSwap hook (direct item-to-item swap detection) ---

    using VisualEquipSwapFn = __int64(__fastcall *)(__int64 *, __int64 *, __int64 **, __int64 **);

    __int64 __fastcall on_visual_equip_swap(
        __int64 *a1, __int64 *a2, __int64 **a3, __int64 **a4);

    void set_visual_equip_swap_trampoline(VisualEquipSwapFn original);

    /** @brief Returns true (once) if a visual equip change was detected. */
    bool consume_equip_change() noexcept;

} // namespace EquipHide
