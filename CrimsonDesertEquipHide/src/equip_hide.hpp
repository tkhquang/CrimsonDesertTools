#pragma once

#include <DetourModKit/abi/wheel_host.h>
#include <DetourModKit/error.hpp>
#include <DetourModKit/session.hpp>

namespace EquipHide
{
    /**
     * @brief Binds config, resolves every anchor, installs the hooks, and brings the input engine live.
     * @param session The live Session. Hotkey guards go into its input scope, and the INI loads through its registry
     *        handle, so ~Session releases both in the right order.
     * @param wheel_host The dev loader's resident wheel host, or nullptr. A non-null table moves a user-bound
     *        mouse-wheel combo's permanent module keepalive onto the loader, which is what lets a logic generation
     *        unmap. The production ASI passes nullptr and uses the local message-hook backend.
     * @return An empty Result on success, or the ErrorCode that stopped init.
     * @note Setup/control-plane only: runs on the bootstrap worker (production) or the loader's control thread (dev),
     *       never under the loader lock.
     */
    [[nodiscard]] DetourModKit::Result<void>
    init(DetourModKit::Session &session, const WheelHostTable *wheel_host = nullptr);

    /**
     * @brief Drains the workers, restores the engine state this mod changed, and tears every hook down newest-first.
     * @return true when every hooked prologue was proved restored. A false return is an unmap REFUSAL: a pinned
     *         backend can still route a call into this image, so the dev loader must keep the DLL mapped.
     * @note Setup/control-plane only. Call it before the Session is destroyed, while this module's code pages are
     *       still mapped.
     */
    [[nodiscard]] bool shutdown();

    /** @brief Arm the post-toggle cascade guard for body parts. */
    void arm_flush_guard() noexcept;

} // namespace EquipHide
