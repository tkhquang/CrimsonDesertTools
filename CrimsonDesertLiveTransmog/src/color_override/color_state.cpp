#include "color_state.hpp"

#include <array>
#include <chrono>

namespace Transmog::ColorOverride::State
{
    namespace
    {
        std::atomic<int> g_activeApplySlot{-1};
        std::atomic<std::int64_t> g_activeApplyValidUntilMs{0};
        std::array<std::atomic<bool>, k_slotCount> g_swatchFrozen{};
        std::atomic<std::int64_t> g_blockPublisherInsertsUntilMs{0};
        std::array<std::atomic<std::int64_t>, k_slotCount> g_hashSetLastAddMs{};
    }

    std::int64_t now_ms() noexcept
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch())
            .count();
    }

    std::atomic<int> &active_apply_slot() noexcept { return g_activeApplySlot; }
    std::atomic<std::int64_t> &active_apply_valid_until_ms() noexcept
    {
        return g_activeApplyValidUntilMs;
    }
    std::atomic<bool> &swatch_frozen(int slot) noexcept
    {
        return g_swatchFrozen[static_cast<std::size_t>(slot)];
    }
    std::atomic<std::int64_t> &block_publisher_inserts_until_ms() noexcept
    {
        return g_blockPublisherInsertsUntilMs;
    }
    std::atomic<std::int64_t> &hash_set_last_add_ms(int slot) noexcept
    {
        return g_hashSetLastAddMs[static_cast<std::size_t>(slot)];
    }
}
