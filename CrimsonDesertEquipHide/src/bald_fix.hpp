#pragma once

#include <cstdint>

namespace EquipHide
{
    /**
     * @brief Postfix rule evaluator hook callback.
     * @details Suppresses hair-hiding rules when Helm/Cloak is hidden to
     *          prevent baldness.  Respects the PlayerOnly flag via a
     *          thread-local actor identity set by the context-create hook.
     */
    using PostfixEvalFn = __int64(__fastcall *)(__int64, __int64);

    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context);
    void set_postfix_eval_trampoline(PostfixEvalFn original);

    /**
     * @brief Per-actor context creator wrapper.
     * @details Sets a thread-local player flag around the original call so
     *          PostfixEval can discriminate player from NPC evaluations.
     *          The PostfixEval context is shared between all actors — the
     *          only per-actor identity lives in this function's parameters.
     */
    using PostfixCtxCreateFn = __int64(__fastcall *)(__int64, __int64 *,
                                                     char, __int64 *,
                                                     __int64 *);

    __int64 __fastcall on_postfix_ctx_create(__int64 a1, __int64 *a2,
                                             char a3, __int64 *a4,
                                             __int64 *a5);
    void set_postfix_ctx_create_trampoline(PostfixCtxCreateFn original);

    /** @brief Cache the main game thread ID for player/NPC discrimination. */
    void set_baldfix_main_thread_id(unsigned long id);

} // namespace EquipHide
