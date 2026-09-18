#ifndef EQUIPHIDE_GLIDING_FIX_HPP
#define EQUIPHIDE_GLIDING_FIX_HPP

#include <cstdint>

namespace EquipHide
{
    /** @brief PartAddShow trampoline signature: the engine's direct-show bypass for a part. */
    using PartAddShowFn =
        __int64(__fastcall *)(__int64, uint8_t, uint64_t, float, __int64, __int64, __int64, __int64, __int64);

    /**
     * @brief PartAddShow detour. Stops a hidden part from a flash during a state transition such as a gliding exit.
     * @details The engine bypasses the vis check on this path and calls the show handler directly. The mod
     *          intercepts it and suppresses a hidden part.
     * @param a1 Engine show context, passed straight through.
     * @param a2 Engine show flags, passed straight through.
     * @param part_hash_ptr Pointer to the part hash the engine shows.
     * @param blend Blend weight, passed straight through.
     * @param a5 Engine show context, passed straight through.
     * @param a6 Engine show context, passed straight through.
     * @param a7 Engine show context, passed straight through.
     * @param a8 Engine show context, passed straight through.
     * @param a9 Engine show context, passed straight through.
     * @return The original handler's result, 0 for a suppressed part, or 0 once the hook drops its trampoline.
     */
    __int64 __fastcall on_part_add_show(
        __int64 a1,
        uint8_t a2,
        uint64_t part_hash_ptr,
        float blend,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8,
        __int64 a9
    );

    /** @brief Store the original function trampoline after hook installation. */
    void set_part_add_show_trampoline(PartAddShowFn original);

} // namespace EquipHide

#endif // EQUIPHIDE_GLIDING_FIX_HPP
