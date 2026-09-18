#ifndef TRANSMOG_TRANSMOG_HPP
#define TRANSMOG_TRANSMOG_HPP

#include <DetourModKit/abi/wheel_host.h>
#include <DetourModKit/error.hpp>
#include <DetourModKit/session.hpp>

#include <cstddef>

namespace Transmog
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

    /** @brief Schedule a full transmog apply across every active slot via the debounce worker. */
    void manual_apply();

    /**
     * @brief Schedule a single-slot transmog apply via the debounce worker.
     * @param slotIdx TransmogSlot index in [0, k_slotCount). The call tears down and re-applies only this slot. Every
     *        other slot stays untouched.
     */
    void manual_apply_slot(std::size_t slotIdx);

    /** @brief Schedule a transmog clear across every slot via the debounce worker. */
    void manual_clear();

    /**
     * @brief Snapshot the character's equipped items into slot_mappings and their live dye into the active preset.
     * @details Clears every session-only prefab pick first, so the captured carrier item IDs become the visible
     *          state. Skips disabled slots.
     */
    void capture_outfit();

    /// Capture current real equipment into slot_mappings (from entry table, not transmog state).
    void capture_real_equipment();

    /**
     * @brief Copy the engine's live dye records for `slotIdx` into the active preset's per-slot dye[] array.
     * @param slotIdx TransmogSlot index in [0, k_slotCount). The call finds the auth-table entry whose gameTag maps
     *        to it.
     * @return true when the call captured at least one channel.
     * @details The copy replaces whatever the preset stored. On success it sets `dyeSparse=true` and
     *          `dye_dirty()=true`. The per-slot "sync from live" button calls this when the user applies in-game dye
     *          to a fake-bound slot's underlying real item and wants those bytes saved into the active preset. The
     *          call does nothing when no preset is active, the slot is disabled, or the auth-table entry is missing.
     */
    [[nodiscard]] bool sync_live_dye_for_slot(std::size_t slotIdx) noexcept;

    /**
     * @brief True once the WorldSystem chain resolves to a live player component.
     * @details An overlay button must gate on this call. The gate stops the log filling with "player not found"
     *          before the first world load.
     */
    [[nodiscard]] bool is_world_ready() noexcept;

} // namespace Transmog

#endif // TRANSMOG_TRANSMOG_HPP
