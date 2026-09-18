#ifndef EQUIPHIDE_BACKGROUND_THREADS_HPP
#define EQUIPHIDE_BACKGROUND_THREADS_HPP

#include <cstdint>

namespace EquipHide
{
    /**
     * @brief Minimum interval between lazy probe signals (ms).
     * @details The EquipVisCheck mid-hook that arms the signal enforces it on the producer side. The probe worker
     *          must consume the signal by value and never reset it. The producer reads a zeroed signal as "never
     *          armed" and re-arms immediately, which drops the real interval to LAZY_PROBE_TICK_MS.
     */
    inline constexpr int64_t LAZY_PROBE_INTERVAL_MS = 60'000;

    /**
     * @brief Lazy probe worker wake period (ms).
     * @details This is not the probe interval. LAZY_PROBE_INTERVAL_MS above is, and how often the signal advances
     *          enforces it. This value only sets how promptly the worker notices a freshly armed signal. A tick that
     *          finds the signal unchanged costs one relaxed atomic load and goes back to sleep.
     */
    inline constexpr int64_t LAZY_PROBE_TICK_MS = 5'000;

    /**
     * @brief Controlled-actor watcher poll interval (ms).
     * @details The poll thread reads the cheap WS->AM->UA->+0xD8 chain on each tick and fires the full
     *          resolve_player_vis_ctrls only once the pointer rotates (a save load, a character swap, or a
     *          controlled-actor teardown). The interval therefore bounds the worst-case latency between a swap and
     *          the rebuilt vis_ctrl list.
     * @note The poll runs independently of the EquipVisCheck hook. The hook alone does not drive cold-load and swap
     *       detection, so the lazy resolve inside check_player_filter is not sufficient on its own.
     */
    inline constexpr int64_t RESOLVE_POLL_INTERVAL_MS = 1'000;

    /** @brief Launch the deferred IndexedStringA scan thread (if pending). */
    void launch_deferred_scan() noexcept;

    /** @brief Launch the lazy re-probe thread for demand-loaded entries (if pending). */
    void launch_lazy_probe() noexcept;

    /**
     * @brief Launch the resolver poll thread. Safe to call multiple times. Only the first call starts the thread.
     */
    void launch_resolve_poll() noexcept;

    /** @brief Join all background threads and clean up. */
    void join_background_threads();

} // namespace EquipHide

#endif // EQUIPHIDE_BACKGROUND_THREADS_HPP
