#pragma once

#include <cstdint>

namespace EquipHide
{
    /**
     * @brief PartInOut direct-show bypass hook callback.
     * @details Prevents hidden parts from flashing during state transitions (e.g. gliding exit).
     *          This function bypasses the vis check and calls the show handler directly,
     *          so we intercept it to suppress hidden parts.
     */
    using PartAddShowFn = __int64(__fastcall *)(__int64, uint8_t, uint64_t,
                                                float, __int64, __int64,
                                                __int64, __int64, __int64);

    __int64 __fastcall on_part_add_show(
        __int64 a1, uint8_t a2, uint64_t partHashPtr, float blend,
        __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9);

    /** @brief Store the original function trampoline after hook installation. */
    void set_part_add_show_trampoline(PartAddShowFn original);

} // namespace EquipHide
