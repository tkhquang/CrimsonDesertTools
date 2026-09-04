#pragma once

#include <cstdint>

namespace EquipHide
{
    /**
     * @brief Minimum interval between lazy probe signals (ms).
     *
     *  Enforced on the producer side, by the EquipVisCheck mid-hook that arms the signal. The probe worker must consume
     *  the signal by value and never reset it: the producer treats a zeroed signal as "never armed" and re-arms
     *  immediately, which drops the real interval to k_lazyProbeTickMs.
     */
    inline constexpr int64_t k_lazyProbeIntervalMs = 60'000;

    /**
     * @brief Lazy probe worker wake period (ms).
     *
     *  Not the probe interval -- that is k_lazyProbeIntervalMs above, enforced by how often the signal advances. This
     *  is only how promptly the worker notices a freshly armed signal; a tick that finds the signal unchanged costs one
     *  relaxed atomic load and goes back to sleep.
     */
    inline constexpr int64_t k_lazyProbeTickMs = 5'000;

    /**
     * @brief Controlled-actor watcher poll interval (ms).
     *
     *  The poll thread reads the cheap WS->AM->UA->+0xD8 chain on each tick and fires the full resolve_player_vis_ctrls
     *  only when the pointer has rotated (save load, character swap, or controlled-actor teardown). The interval
     *  therefore bounds the worst-case latency between a swap and the visCtrl list rebuilding against the new actor; 1s
     *  matches LT's load_detect_thread cadence.
     *
     *  Runs independently of the EquipVisCheck hook: on v1.04.00 the hook fires on a narrower event set than on
     *  v1.03.01, so the lazy resolve inside check_player_filter is not sufficient on its own to drive cold-load and
     *  swap detection.
     */
    inline constexpr int64_t k_resolvePollIntervalMs = 1'000;

    /** @brief Launch the deferred IndexedStringA scan thread (if pending). */
    void launch_deferred_scan() noexcept;

    /** @brief Launch the lazy re-probe thread for demand-loaded entries (if pending). */
    void launch_lazy_probe() noexcept;

    /**
     * @brief Launch the resolver poll thread. Safe to call multiple times; only the first call actually starts the
     *        thread.
     */
    void launch_resolve_poll() noexcept;

    /** @brief Join all background threads and clean up. */
    void join_background_threads();

} // namespace EquipHide
