#ifndef EQUIPHIDE_BALD_FIX_HPP
#define EQUIPHIDE_BALD_FIX_HPP

#include <cstdint>

namespace EquipHide
{
    /** @brief PostfixEval trampoline signature: the engine's postfix rule evaluator. */
    using PostfixEvalFn = __int64(__fastcall *)(__int64, __int64);

    /**
     * @brief PostfixEval detour. Keeps the hair when a Helm, Cloak or Mask is hidden.
     * @details Temporarily overrides item priority bitmasks so PostfixEval treats a head-covering item as inactive
     *          and no hair rule matches. It applies to the player context only.
     * @param ruleObj The rule object the engine evaluates.
     * @param context The equipped-item context the rule reads.
     * @return The original evaluator's result, or 0 once the hook drops its trampoline.
     */
    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context);

    /** @brief Store the original function trampoline after hook installation. */
    void set_postfix_eval_trampoline(PostfixEvalFn original);

} // namespace EquipHide

#endif // EQUIPHIDE_BALD_FIX_HPP
