#ifndef TRANSMOG_PARAM_SLOT_GUARD_HPP
#define TRANSMOG_PARAM_SLOT_GUARD_HPP

#include <DetourModKit/hook.hpp>

/**
 * @file param_slot_guard.hpp
 * @brief Guards ComponentParamSlot against a null slot table, the read that crashes an interaction in transmogged
 *        Gadget_Gloves.
 *
 * An interaction effect finds a part on the player body by key and loads the parameter-slot table of the part
 * component from `component+0x228`. It passes that table to ComponentParamSlot without a null check. A part that LT
 * built from another item's prefab carries no table, so the slot read faults on the main thread.
 *
 * Every caller of ComponentParamSlot null-checks the returned slot and skips its effect on null. The guard returns
 * null for a null table, so the caller takes that skip path. A non-null table reaches the engine body unchanged.
 */

namespace Transmog::param_slot_guard
{
    /**
     * @brief Resolves ComponentParamSlot and installs the counted guard hook on its entry.
     * @return true when the hook is armed. A false return leaves the engine unguarded and logs the cause.
     * @note Setup/control-plane only: the install allocates the trampoline. Call it once per Logic DLL generation.
     */
    [[nodiscard]] bool install(DetourModKit::hook::HookStack &hooks) noexcept;
} // namespace Transmog::param_slot_guard

#endif // TRANSMOG_PARAM_SLOT_GUARD_HPP
