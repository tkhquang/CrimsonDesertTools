#include "gliding_fix.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>

namespace EquipHide
{
    namespace
    {
        std::atomic<PartAddShowFn> s_originalPartAddShow{nullptr};

        // SEH guards the one raw dereference inside check_part_hidden. A stray part-hash pointer must not crash the
        // game on a per-part detour path.
        bool should_skip_part_add_show(uint64_t partHashPtr) noexcept
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
    } // namespace

    void set_part_add_show_trampoline(PartAddShowFn original)
    {
        s_originalPartAddShow.store(original, std::memory_order_relaxed);
    }

    __int64 __fastcall on_part_add_show(
        __int64 a1,
        uint8_t a2,
        uint64_t partHashPtr,
        float blend,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8,
        __int64 a9
    )
    {
        if (flag_gliding_fix().load(std::memory_order_relaxed) && should_skip_part_add_show(partHashPtr))
            return 0;
        // Snapshot the trampoline. A stray callback fired between the shutdown's remove_hook(), which restores the
        // prologue bytes and disables the detour, and the DLL unmap observes a null trampoline. A tail call through
        // it crashes before the loader can free the page. The safe-default return mirrors the early-out skip path.
        const auto trampoline = s_originalPartAddShow.load(std::memory_order_relaxed);
        if (!trampoline)
            return 0;
        return trampoline(a1, a2, partHashPtr, blend, a5, a6, a7, a8, a9);
    }

} // namespace EquipHide
