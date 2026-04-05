#pragma once

#include <cstdint>

namespace EquipHide
{
    using PostfixEvalFn = __int64(__fastcall *)(__int64, __int64);

    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context);
    void set_postfix_eval_trampoline(PostfixEvalFn original);

} // namespace EquipHide
