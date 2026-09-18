#pragma once

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

    void manual_apply();

    /**
     * @brief Schedule a single-slot transmog apply via the debounce worker.
     * @param slotIdx TransmogSlot index (0..4). Only this slot is torn down and re-applied; other slots are untouched.
     */
    void manual_apply_slot(std::size_t slotIdx);

    void manual_clear();
    void capture_outfit();

    /// Capture current real equipment into slot_mappings (from entry table, not transmog state).
    void capture_real_equipment();

    /**
     * Read the engine's live dye records for the auth-table entry whose gameTag maps to `slotIdx` and write them into
     * the active preset's per-slot dye[] array (replacing whatever was stored). Sets `dyeSparse=true` and
     * `dye_dirty()=true` on success. Used by the per-slot "sync from live" button when the user has applied in-game dye
     * to a fake-bound slot's underlying real item and wants those bytes saved into the active preset. Returns true if
     * at least one channel was captured. No-op when no active preset, slot disabled, or auth-table entry missing.
     */
    bool sync_live_dye_for_slot(std::size_t slotIdx) noexcept;

    /**
     * Returns true once the WorldSystem chain resolves to a live player component. Overlay buttons should gate on this
     * to avoid spamming "player not found" before the first world load.
     */
    bool is_world_ready() noexcept;

} // namespace Transmog
