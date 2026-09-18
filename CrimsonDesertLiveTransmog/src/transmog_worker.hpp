#ifndef TRANSMOG_TRANSMOG_WORKER_HPP
#define TRANSMOG_TRANSMOG_WORKER_HPP

#include <cstdint>

namespace Transmog
{
    // Debounce timing

    inline constexpr std::uint64_t APPLY_DEBOUNCE_MS = 1500;
    inline constexpr std::uint64_t MANUAL_DEBOUNCE_MS = 100;

    // Debounce window that replaces MANUAL_DEBOUNCE_MS once requests start arriving in a run.
    //
    // A request that arrives within this window of the PREVIOUS REQUEST belongs to a burst and re-arms the deadline
    // this far out. While the clicks continue the deadline keeps moving and nothing is built. Once they stop, ONE
    // apply runs and reads live state, so only the preset the user landed on reaches the body. A lone switch keeps
    // the short debounce and applies at once.
    //
    // The value sits above the measured 300-400 ms click spacing so an ordinary burst coalesces. A larger value
    // absorbs slower clicks and costs a longer wait after the last one.
    inline constexpr std::uint64_t BURST_COALESCE_MS = 500;

    // Player component resolution

    /**
     * @brief Walks the WorldSystem pointer chain to the player's equipment component (a1 for SlotPopulator).
     * @return The component pointer, or 0 when any link in the chain is null or invalid (pre-world, loading screen).
     */
    __int64 resolve_player_component() noexcept;

    // Debounce worker

    /**
     * @brief Bumps the debounce deadline forward by @p debounce_ms and kicks the persistent worker.
     * @param debounce_ms Quiet window the burst must clear before the apply runs.
     * @details Multiple rapid calls collapse into a single apply or clear once the burst stays quiet for the window.
     */
    void schedule_transmog_ms(std::uint64_t debounce_ms);

    /**
     * @brief Hook-thread entry point for a scheduled apply.
     * @param a1 Equip component the hook observed. Dropped on purpose.
     * @param targetId Item the hook observed. Dropped on purpose.
     * @details The worker re-resolves both from authoritative state at apply time.
     */
    void schedule_transmog(__int64 a1, std::uint16_t targetId);

    /// Starts the persistent apply worker if it is not already running.
    void ensure_apply_worker_started();

    /// Requests stop and joins the persistent apply worker.
    void stop_apply_worker();

    // Load-detection thread

    /// Starts the load-detection thread that watches for world reloads and character swaps.
    void start_load_detect_thread();

    /// Requests stop and joins the load-detection thread.
    void stop_load_detect_thread();

    // Deferred nametable scan

    /// Launches the background worker that builds the item-name catalog once the engine publishes it.
    void launch_deferred_nametable_scan() noexcept;

    /// Requests stop and joins the deferred nametable scan worker.
    void join_deferred_nametable_scan();

    // Deferred part_show_suppress slot-hash scan
    //
    // IndexedStringA carries the `CD_Helm` / `CD_Upperbody` / ... part names part_show_suppress keys on. The table is
    // populated by the engine during world load. An LT load at cold-launch (before the main menu finishes its wiring)
    // otherwise leaves part_show_suppress inert for the whole session, because the synchronous scan at LT init observes
    // a near-empty table. The deferred worker mirrors the nametable pattern: poll for world-ready, scan until
    // every expected slot hash is present, then call init_slot_hashes once to commit.

    /// Launches the background worker that resolves the part_show_suppress slot hashes.
    void launch_deferred_slot_hash_scan() noexcept;

    /// Requests stop and joins the deferred slot-hash scan worker.
    void join_deferred_slot_hash_scan();

    // Targeted-apply redirect

    /**
     * @brief Redirects the next scheduled apply onto the editing character's body.
     * @param char_idx 1-based protagonist index of the editing character. Pass 0 to clear a pending redirect without
     *                a schedule.
     * @details Overlay-UI entry points (manual_apply, manual_apply_slot, manual_clear, picker changes, preset cycles)
     *          call this when the user pins the editing dropdown to a non-controlled character and
     *          `flag_apply_to_editing` is on. The worker consumes the idx once. Engine-triggered applies fall through
     *          to the default controlled-body path.
     */
    void set_targeted_apply_char_idx(std::uint32_t char_idx) noexcept;

    /**
     * @brief Reads the pending targeted-apply redirect without consuming it.
     * @return The pending 1-based protagonist index, or 0 when no redirect is armed.
     * @details Entry points use it to skip the schedule entirely when the editing character is not live, because the
     *          worker then falls back to the controlled body, which is the legacy cross-body behavior the user opted
     *          out of.
     */
    [[nodiscard]] std::uint32_t pending_targeted_apply_char_idx() noexcept;

} // namespace Transmog

#endif // TRANSMOG_TRANSMOG_WORKER_HPP
