#ifndef TRANSMOG_COLOR_OVERRIDE_ONE_SHOT_LOG_SET_HPP
#define TRANSMOG_COLOR_OVERRIDE_ONE_SHOT_LOG_SET_HPP

// Bounded-capacity lock-free dedup set for one-shot diagnostic logs.
//
// Use case: hot-path callers want to log the first sighting of each distinct token / vtable / (slot, hash) pair without
// flooding the log on every fire. They probe via `insert_unique`; only the first caller for a given value sees `true`.
//
// Trade-offs (intentional, suit diagnostic logging only):
//   * SCAN-then-RESERVE is NOT atomic. Two threads can both miss in the scan and both reserve a slot for the same value
//     - net effect is a duplicate log line, never a corruption. Acceptable for one-shot logs; do NOT use for state
//     that must be unique.
//   * `compare_exchange_weak` is used for the reserve step (no `fetch_add + rollback-on-overflow` dance). Once
//     saturated, all further inserts cleanly return false.
//   * `clear()` writes count=0 first so concurrent readers see "empty" during the wipe rather than a stale-tail view.
//
// Header-only; instantiate as `OneShotLogSet<u32, 64> g_set;`.

#include <array>
#include <atomic>
#include <cstddef>

namespace Transmog::color_override
{
    template <typename T, std::size_t N> class OneShotLogSet
    {
    public:
        /**
         * Try to record `value` as a first-time sighting. Returns true only for the first caller per distinct value
         * (modulo the scan-vs-reserve race noted above - duplicates are possible but bounded).
         */
        bool insert_unique(T value) noexcept
        {
            // Snapshot count once.
            std::size_t cnt = m_count.load(std::memory_order_acquire);
            const std::size_t upper = (cnt < N) ? cnt : N;
            for (std::size_t i = 0; i < upper; ++i)
                if (m_set[i].load(std::memory_order_relaxed) == value)
                    return false;

            // Reserve a slot via compare-exchange. Refresh cnt on each retry. Avoids the `fetch_add +
            // fetch_sub-on-overflow` pattern which leaves the count transiently above N.
            for (;;)
            {
                cnt = m_count.load(std::memory_order_acquire);
                if (cnt >= N)
                    return false;
                if (m_count.compare_exchange_weak(cnt, cnt + 1, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    m_set[cnt].store(value, std::memory_order_release);
                    return true;
                }
            }
        }

        /**
         * Wipe the set so the next session's first sightings re-log. Writes count=0 BEFORE zeroing entries so a
         * concurrent `insert_unique` sees empty rather than a stale tail.
         */
        void clear() noexcept
        {
            const std::size_t prev = m_count.load(std::memory_order_acquire);
            m_count.store(0, std::memory_order_release);
            const std::size_t upper = (prev < N) ? prev : N;
            for (std::size_t i = 0; i < upper; ++i)
                m_set[i].store(T{}, std::memory_order_relaxed);
        }

        /**
         * Current size (diagnostic only - may transiently lag concurrent inserts).
         */
        std::size_t size() const noexcept { return m_count.load(std::memory_order_acquire); }

    private:
        std::array<std::atomic<T>, N> m_set{};
        std::atomic<std::size_t> m_count{0};
    };
} // namespace Transmog::color_override

#endif // TRANSMOG_COLOR_OVERRIDE_ONE_SHOT_LOG_SET_HPP
