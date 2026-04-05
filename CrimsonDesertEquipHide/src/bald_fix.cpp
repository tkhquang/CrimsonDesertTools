#include "bald_fix.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    static PostfixEvalFn s_originalPostfixEval = nullptr;

    void set_postfix_eval_trampoline(PostfixEvalFn original)
    {
        s_originalPostfixEval = original;
    }

    /**
     * @brief Hair-hiding suffix check.
     *
     * ruleObj+0x18 is a string handle (char**); double-deref for char*.
     * Hair-hiding suffixes: _a _c _d _f _i _q _v
     */
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

    /* Player contexts have a populated owner pointer at +0x50 and non-zero
       item count at +0x60.  NPC contexts are empty (null owner, zero items)
       so PostfixEval returns 0 for them regardless, but we guard explicitly
       to avoid suppressing rules for contexts we did not originate. */
    static bool is_player_context(__int64 context) noexcept
    {
        auto ctx = static_cast<uintptr_t>(context);
        if (*reinterpret_cast<const uintptr_t *>(ctx + 0x50) == 0)
            return false;
        if (*reinterpret_cast<const uint32_t *>(ctx + 0x60) == 0)
            return false;
        return true;
    }

    static bool should_suppress_hair_hiding(__int64 ruleObj,
                                            __int64 context) noexcept
    {
        if (!is_hair_hiding_rule(ruleObj))
            return false;

        if (!is_category_hidden(Category::Helm) &&
            !is_category_hidden(Category::Cloak))
            return false;

        /* When PlayerOnly is enabled, only suppress for player contexts.
           When disabled, suppress for all contexts (player + NPC). */
        if (flag_player_only().load(std::memory_order_relaxed) &&
            !is_player_context(context))
            return false;

        return true;
    }

    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context)
    {
        if (flag_bald_fix().load(std::memory_order_relaxed))
        {
            __try
            {
                if (should_suppress_hair_hiding(ruleObj, context))
                {
                    static std::atomic<bool> s_logged{false};
                    if (!s_logged.exchange(true, std::memory_order_relaxed))
                        DMK::Logger::get_instance().debug(
                            "BaldFix: suppressed hair-hiding rule");
                    return 0;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return s_originalPostfixEval(ruleObj, context);
    }

} // namespace EquipHide
