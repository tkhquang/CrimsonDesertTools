#pragma once

#include <cstdint>

namespace EquipHide
{
    /** @brief Minimum interval between lazy probe signals (ms). */
    inline constexpr int64_t k_lazyProbeIntervalMs = 60'000;

    /** @brief Launch the deferred IndexedStringA scan thread (if pending). */
    void launch_deferred_scan() noexcept;

    /** @brief Launch the lazy re-probe thread for demand-loaded entries (if pending). */
    void launch_lazy_probe() noexcept;

    /** @brief Join all background threads and clean up. */
    void join_background_threads();

} // namespace EquipHide
