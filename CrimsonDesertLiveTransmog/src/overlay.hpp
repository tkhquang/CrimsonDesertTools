#ifndef TRANSMOG_OVERLAY_HPP
#define TRANSMOG_OVERLAY_HPP

#include <Windows.h>

namespace Transmog
{
    /**
     * @brief Brings up the transmog UI, preferring a ReShade addon tab and falling back to a standalone window.
     *
     * The standalone path spawns a background render thread that waits for the game world to load, then creates a
     * transparent overlay window with a D3D11 WARP device for swap-chain-free rendering via GDI blit.
     *
     * @return true if either path came up.
     * @note Takes no module handle on purpose. ReShade addon registration needs the handle of the module the overlay
     *       code itself lives in, and only this module can name that: a caller in the same DLL has to derive it from
     *       a lifecycle accessor that is null on some paths, and ReShade latches the first value it is given for the
     *       process. Resolving it here makes the production ASI and the dev logic DLL take the identical path.
     */
    [[nodiscard]] bool init_overlay();

    /**
     * @brief Signal the overlay render thread to shut down and wait for it to exit. Safe to call if init failed.
     */
    void shutdown_overlay() noexcept;

    /** @brief Force the standalone overlay even when ReShade is available. */
    void set_force_standalone(bool force) noexcept;

    /**
     * @brief Render one frame of the transmog UI inside an ImGui window.
     *
     * Called from the overlay render thread between NewFrame and Render. Creates its own ImGui::Begin/End window.
     */
    void draw_overlay();

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_HPP
