#include "gliding_fix.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    static PartAddShowFn s_originalPartAddShow = nullptr;

    void set_part_add_show_trampoline(PartAddShowFn original)
    {
        s_originalPartAddShow = original;
    }

    static bool should_skip_part_add_show(uint64_t partHashPtr) noexcept
    {
        __try
        {
            return check_part_hidden(partHashPtr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __int64 __fastcall on_part_add_show(
        __int64 a1, uint8_t a2, uint64_t partHashPtr, float blend,
        __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9)
    {
        if (flag_gliding_fix().load(std::memory_order_relaxed) &&
            should_skip_part_add_show(partHashPtr))
            return 0;
        return s_originalPartAddShow(a1, a2, partHashPtr, blend,
                                     a5, a6, a7, a8, a9);
    }

} // namespace EquipHide
