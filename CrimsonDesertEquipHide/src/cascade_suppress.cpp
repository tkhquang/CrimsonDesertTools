#include "cascade_suppress.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    static std::atomic<bool> s_equipChangeDetected{false};

    // Slot ID for chest armor (confirmed via log: slot=4, item=5208).
    // Only chest changes need cascade re-sync -- other armor slots
    // (necklace=9, mask=18, etc.) don't affect chest lock state.
    static constexpr uint16_t k_chestSlot = 4;

    // --- VisualEquipChange hook (equip/unequip) ---

    static VisualEquipChangeFn s_originalVisualEquipChange = nullptr;

    void set_visual_equip_change_trampoline(VisualEquipChangeFn original)
    {
        s_originalVisualEquipChange = original;
    }

    __int64 __fastcall on_visual_equip_change(
        __int64 bodyComp, int16_t slotId, int16_t itemId, __int64 itemData)
    {
        DMK::Logger::get_instance().trace(
            "VisualEquipChange: slot={} item={}", slotId, itemId);

        if (flag_cascade_fix().load(std::memory_order_relaxed) &&
            is_category_hidden(Category::Chest) &&
            slotId == k_chestSlot)
        {
            DMK::Logger::get_instance().debug(
                "VisualEquipChange: chest slot={} item={} -- clearing cascade locks",
                slotId, itemId);
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
        __try
        {
            // Log all swapped slots at trace level for future reference.
            auto &logger = DMK::Logger::get_instance();
            __int64 *iter = a4 ? (*a4 ? *a4 : a4[1]) : nullptr;
            if (iter)
            {
                auto base = *iter;
                auto count = *reinterpret_cast<const uint32_t *>(
                    reinterpret_cast<const char *>(iter) + 8);
                bool hasChest = false;
                for (uint32_t i = 0; i < count && i < 16; ++i)
                {
                    auto slot = *reinterpret_cast<const uint16_t *>(
                        base + 232 * i + 208);
                    logger.trace("EquipSwap: slot={}", slot);
                    if (slot == k_chestSlot)
                        hasChest = true;
                }

                if (hasChest &&
                    flag_cascade_fix().load(std::memory_order_relaxed) &&
                    is_category_hidden(Category::Chest))
                {
                    logger.debug(
                        "EquipSwap: chest slot detected -- signalling re-sync");
                    s_equipChangeDetected.store(true,
                                                std::memory_order_relaxed);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return s_originalVisualEquipSwap(a1, a2, a3, a4);
    }

    bool consume_equip_change() noexcept
    {
        return s_equipChangeDetected.exchange(false, std::memory_order_relaxed);
    }

} // namespace EquipHide
