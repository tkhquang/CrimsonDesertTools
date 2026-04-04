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
     * ruleObj + 0x18 is a string handle (char**). Double-deref to get char*.
     * Hair-hiding suffixes: _a, _c, _d, _f, _i, _q, _v
     */
    static bool is_hair_hiding_rule(__int64 ruleObj)
    {
        auto handleAddr = *reinterpret_cast<uintptr_t *>(ruleObj + 0x18);
        if (handleAddr < 0x10000)
            return false;
        auto suffixAddr = *reinterpret_cast<uintptr_t *>(handleAddr);
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

    static bool should_suppress_hair_hiding(__int64 ruleObj) noexcept
    {
        __try
        {
            if (!is_hair_hiding_rule(ruleObj))
                return false;
            return is_category_hidden(Category::Helm) ||
                   is_category_hidden(Category::Cloak);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context)
    {
        if (flag_bald_fix().load(std::memory_order_relaxed) &&
            should_suppress_hair_hiding(ruleObj))
        {
            static std::atomic<bool> s_loggedBaldFix{false};
            if (!s_loggedBaldFix.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().debug(
                    "BaldFix: suppressed hair-hiding rule (suffix at ruleObj 0x{:X})",
                    ruleObj);
            return 0;
        }
        return s_originalPostfixEval(ruleObj, context);
    }

} // namespace EquipHide
