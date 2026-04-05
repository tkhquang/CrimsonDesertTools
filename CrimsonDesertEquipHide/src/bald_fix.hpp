#pragma once

#include <cstdint>

namespace EquipHide
{
    /**
     * @brief Postfix rule evaluator hook callback.
     * @details Temporarily overrides item priority bitmasks so PostfixEval
     *          treats head-covering items as inactive, preventing hair-hiding
     *          when Helm/Cloak/Mask is hidden.  Only applies to the player context.
     */
    using PostfixEvalFn = __int64(__fastcall *)(__int64, __int64);

    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context);

    /** @brief Store the original function trampoline after hook installation. */
    void set_postfix_eval_trampoline(PostfixEvalFn original);

} // namespace EquipHide
