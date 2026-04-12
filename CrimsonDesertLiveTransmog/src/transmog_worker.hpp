#pragma once

#include <cstdint>

namespace Transmog
{
    // --- Debounce timing ---

    inline constexpr std::uint64_t k_applyDebounceMs = 1500;
    inline constexpr std::uint64_t k_manualDebounceMs = 100;

    // --- Player component resolution ---

    /// Walks the WorldSystem pointer chain to resolve the player's
    /// equipment component (a1 for SlotPopulator). Returns 0 if any
    /// link in the chain is null or invalid (pre-world, loading screen).
    __int64 resolve_player_component() noexcept;

    // --- Debounce worker ---

    /// Bumps the debounce deadline forward by @p debounce_ms and kicks
    /// the persistent worker. Multiple rapid calls collapse into a single
    /// apply/clear once the burst has been quiet for the specified window.
    void schedule_transmog_ms(std::uint64_t debounce_ms);

    /// Hook-thread entry point. Arguments are intentionally dropped —
    /// the worker re-resolves both from authoritative state at apply time.
    void schedule_transmog(__int64 a1, std::uint16_t targetId);

    void ensure_apply_worker_started();
    void stop_apply_worker();

    // --- Load-detection thread ---

    void start_load_detect_thread();
    void stop_load_detect_thread();

    // --- Deferred nametable scan ---

    void launch_deferred_nametable_scan() noexcept;
    void join_deferred_nametable_scan();

} // namespace Transmog
