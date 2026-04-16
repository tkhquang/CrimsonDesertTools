#ifndef TRANSMOG_OVERLAY_HPP
#define TRANSMOG_OVERLAY_HPP

#include <Windows.h>

namespace Transmog
{
    /**
     * @brief Initialise the standalone ImGui overlay.
     *
     * Spawns a background render thread that waits for the game world
     * to load, then creates a transparent overlay window with a D3D11
     * WARP device for swap-chain-free rendering via GDI blit.
     *
     * @param hModule Handle of the current DLL module (reserved).
     * @return true if the render thread was started successfully.
     */
    [[nodiscard]] bool init_overlay(HMODULE hModule);

    /**
     * @brief Signal the overlay render thread to shut down and wait
     *        for it to exit.  Safe to call if init failed.
     */
    void shutdown_overlay() noexcept;

    /** @brief Force the standalone overlay even when ReShade is available. */
    void set_force_standalone(bool force);

    /**
     * @brief Render one frame of the transmog UI inside an ImGui window.
     *
     * Called from the overlay render thread between NewFrame and Render.
     * Creates its own ImGui::Begin/End window.
     */
    void draw_overlay();

} // namespace Transmog

#endif // TRANSMOG_OVERLAY_HPP
