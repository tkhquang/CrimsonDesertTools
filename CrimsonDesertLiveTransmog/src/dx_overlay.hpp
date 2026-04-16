#ifndef TRANSMOG_DX_OVERLAY_HPP
#define TRANSMOG_DX_OVERLAY_HPP

#include <cstdint>

namespace Transmog
{
    /**
     * @brief Spawn the overlay render thread.
     *
     * The thread waits for the game world to load, then creates a
     * transparent layered window with a D3D11 WARP device and renders
     * ImGui to an offscreen texture that is composited onto the screen
     * via GDI UpdateLayeredWindow.  No DXGI swap chain is created, so
     * this approach is compatible with any DX wrapper (OptiScaler,
     * ReShade, SpecialK).
     *
     * @return true if the thread was started successfully.
     */
    [[nodiscard]] bool init_dx_overlay();

    /**
     * @brief Signal the render thread to exit and wait for it to finish.
     *
     * Safe to call even if init_dx_overlay was never called or failed.
     */
    void shutdown_dx_overlay() noexcept;

    /**
     * @brief Toggle overlay visibility.
     *
     * Debounced (300 ms cooldown) to prevent key-repeat and polling
     * overlap from rapidly flipping the state.  Thread-safe.
     */
    void toggle_overlay_visible() noexcept;

    /** @brief Query whether the overlay is currently visible. */
    [[nodiscard]] bool is_overlay_visible() noexcept;

} // namespace Transmog

#endif // TRANSMOG_DX_OVERLAY_HPP
