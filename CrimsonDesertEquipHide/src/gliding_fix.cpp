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
        // Snapshot the trampoline. A stray callback fired between the
        // shutdown's remove_hook() (which restores prologue bytes and
        // disables the SafetyHook detour) and the DLL unmap would
        // observe a NULL trampoline; tail-calling through it would
        // crash before the loader can free the page. Safe-default
        // return mirrors the early-out skip path.
        auto trampoline = s_originalPartAddShow;
        if (!trampoline)
            return 0;
        return trampoline(a1, a2, partHashPtr, blend,
                          a5, a6, a7, a8, a9);
    }

} // namespace EquipHide
