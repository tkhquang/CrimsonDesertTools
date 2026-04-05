#include "bald_fix.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstdint>

namespace EquipHide
{
    static PostfixEvalFn s_originalPostfixEval = nullptr;
    static std::atomic<uintptr_t> s_cachedContext{0};

    void set_postfix_eval_trampoline(PostfixEvalFn original)
    {
        s_originalPostfixEval = original;
    }

    // Priority bitmask layout (item+0x70, DWORD):
    //   bits 16-22: "priority level exists"
    //   bits  0-6:  "priority level active"
    //
    // Bit 19 = priority 3 exists.  Without bit 3 (active), PostfixEval
    // sees priority 3 as highest but inactive -> rule doesn't match ->
    // hair stays visible.  Official HideAlways does the same:
    // 0x00010001 -> 0x00090001.

    static constexpr uint32_t k_priorityBit = 0x00080000u; // bit 19
    static constexpr uint32_t k_activeBit   = 0x00000008u; // bit 3
    static constexpr int      k_maxItems    = 128;

    static bool is_hair_hiding_rule(__int64 ruleObj) noexcept
    {
        auto handleAddr = *reinterpret_cast<const uintptr_t *>(ruleObj + 0x18);
        if (handleAddr < 0x10000)
            return false;
        auto suffixAddr = *reinterpret_cast<const uintptr_t *>(handleAddr);
        if (suffixAddr < 0x10000)
            return false;

        const auto *suffix = reinterpret_cast<const char *>(suffixAddr);
        if (suffix[0] != '_' || suffix[2] != '\0')
            return false;

        switch (suffix[1])
        {
        case 'a':
        case 'c':
        case 'd':
        case 'f':
        case 'i':
        case 'q':
        case 'v':
            return true;
        default:
            return false;
        }
    }

    /* Build a bitmask of hidden head-covering categories for per-item
       filtering.  Only items belonging to a hidden category get the
       priority override — items for shown categories keep their original
       bitmask so PostfixEval still hides hair/beard under them. */
    static CategoryMask hidden_headgear_mask() noexcept
    {
        CategoryMask mask = 0;
        if (is_category_hidden(Category::Helm))  mask |= category_bit(Category::Helm);
        if (is_category_hidden(Category::Cloak)) mask |= category_bit(Category::Cloak);
        if (is_category_hidden(Category::Mask))  mask |= category_bit(Category::Mask);
        return mask;
    }

    /* Temporarily set bit 19 on items whose category is hidden, call
       the original PostfixEval, then restore.  Only items belonging to
       a hidden headgear category are overridden — e.g. hiding Mask only
       overrides Mask items (beard stays visible) while Helm items keep
       their bitmask (hair hidden under helmet as normal). */
    static __int64 eval_with_priority_override(
        __int64 ruleObj, __int64 context) noexcept
    {
        auto itemsPtr = *reinterpret_cast<uintptr_t *>(context + 0x58);
        auto itemCount = *reinterpret_cast<uint32_t *>(context + 0x60);

        if (itemsPtr < 0x10000 || itemCount == 0 || itemCount > k_maxItems)
            return s_originalPostfixEval(ruleObj, context);

        auto hiddenMask = hidden_headgear_mask();

        uintptr_t patchedItems[k_maxItems];
        uint32_t  originalValues[k_maxItems];
        int       patchCount = 0;

        auto *itemArray = reinterpret_cast<uintptr_t *>(itemsPtr);

        for (uint32_t i = 0; i < itemCount; ++i)
        {
            auto item = itemArray[i];
            if (item < 0x10000)
                continue;
            if (*reinterpret_cast<uint8_t *>(item + 0x88) == 0)
                continue;

            /* Only override items whose part hash belongs to a hidden
               head-covering category. */
            auto partHash = *reinterpret_cast<uint32_t *>(item + 0x48);
            if (!needs_classification(partHash))
                continue;
            auto partCat = classify_part(partHash);
            if ((partCat & hiddenMask) == 0)
                continue;

            auto *bm = reinterpret_cast<uint32_t *>(item + 0x70);
            uint32_t val = *bm;

            if (val & k_priorityBit)
                continue;

            *bm = (val | k_priorityBit) & ~k_activeBit;
            patchedItems[patchCount] = item;
            originalValues[patchCount] = val;
            ++patchCount;
        }

        __int64 result = s_originalPostfixEval(ruleObj, context);

        for (int i = 0; i < patchCount; ++i)
            *reinterpret_cast<uint32_t *>(patchedItems[i] + 0x70) = originalValues[i];

        return result;
    }

    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context)
    {
        if (flag_bald_fix().load(std::memory_order_relaxed))
        {
            __try
            {
                auto ctx = static_cast<uintptr_t>(context);

                /* Cache the player context on first populated call. */
                if (s_cachedContext.load(std::memory_order_relaxed) == 0 &&
                    ctx > 0x10000)
                {
                    auto cnt = *reinterpret_cast<const uint32_t *>(ctx + 0x60);
                    if (cnt > 0)
                    {
                        s_cachedContext.store(ctx, std::memory_order_relaxed);
                        DMK::Logger::get_instance().debug(
                            "BaldFix: cached context 0x{:X} ({} items)",
                            ctx, cnt);
                    }
                }

                /* Only override for the player context + hair-hiding rules
                   + any head-covering gear hidden.  Per-item category
                   filtering ensures only items for hidden categories are
                   overridden.  NPC contexts have different addresses. */
                if (ctx == s_cachedContext.load(std::memory_order_relaxed) &&
                    is_hair_hiding_rule(ruleObj) &&
                    (is_category_hidden(Category::Helm) ||
                     is_category_hidden(Category::Cloak) ||
                     is_category_hidden(Category::Mask)))
                {
                    static std::atomic<bool> s_logged{false};
                    if (!s_logged.exchange(true, std::memory_order_relaxed))
                        DMK::Logger::get_instance().debug(
                            "BaldFix: priority override active");
                    return eval_with_priority_override(ruleObj, context);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return s_originalPostfixEval(ruleObj, context);
    }

} // namespace EquipHide
