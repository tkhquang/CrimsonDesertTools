#include "cascade_suppress.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    static std::atomic<bool> s_equipChangeDetected{false};

    // --- VisualEquipChange hook (equip/unequip) ---

    static VisualEquipChangeFn s_originalVisualEquipChange = nullptr;

    void set_visual_equip_change_trampoline(VisualEquipChangeFn original)
    {
        s_originalVisualEquipChange = original;
    }

    __int64 __fastcall on_visual_equip_change(
        __int64 bodyComp, int16_t slotId, int16_t itemId, __int64 itemData)
    {
        if (flag_cascade_fix().load(std::memory_order_relaxed) &&
            is_category_hidden(Category::Chest))
        {
            s_equipChangeDetected.store(true, std::memory_order_relaxed);
        }
        return s_originalVisualEquipChange(bodyComp, slotId, itemId, itemData);
    }

    // --- VisualEquipSwap hook (direct item-to-item swap) ---

    static VisualEquipSwapFn s_originalVisualEquipSwap = nullptr;

    void set_visual_equip_swap_trampoline(VisualEquipSwapFn original)
    {
        s_originalVisualEquipSwap = original;
    }

    __int64 __fastcall on_visual_equip_swap(
        __int64 *a1, __int64 *a2, __int64 **a3, __int64 **a4)
    {
        if (flag_cascade_fix().load(std::memory_order_relaxed) &&
            is_category_hidden(Category::Chest))
        {
            s_equipChangeDetected.store(true, std::memory_order_relaxed);
        }
        return s_originalVisualEquipSwap(a1, a2, a3, a4);
    }

    bool consume_equip_change() noexcept
    {
        return s_equipChangeDetected.exchange(false, std::memory_order_relaxed);
    }

} // namespace EquipHide
