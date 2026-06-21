#pragma once

#include <cstdint>

namespace Transmog
{
    // --- Debounce timing ---

    inline constexpr std::uint64_t k_applyDebounceMs = 1500;
    inline constexpr std::uint64_t k_manualDebounceMs = 100;

    // --- Player component resolution ---

    /**
     * Walks the WorldSystem pointer chain to resolve the player's equipment component (a1 for SlotPopulator). Returns 0
     * if any link in the chain is null or invalid (pre-world, loading screen).
     */
    __int64 resolve_player_component() noexcept;

    // --- Debounce worker ---

    /**
     * Bumps the debounce deadline forward by @p debounce_ms and kicks the persistent worker. Multiple rapid calls
     * collapse into a single apply/clear once the burst has been quiet for the specified window.
     */
    void schedule_transmog_ms(std::uint64_t debounce_ms);

    /**
     * Hook-thread entry point. Arguments are intentionally dropped; the worker re-resolves both from authoritative
     * state at apply time.
     */
    void schedule_transmog(__int64 a1, std::uint16_t targetId);

    void ensure_apply_worker_started();
    void stop_apply_worker();

    // --- Load-detection thread ---

    void start_load_detect_thread();
    void stop_load_detect_thread();

    // --- Deferred nametable scan ---

    void launch_deferred_nametable_scan() noexcept;
    void join_deferred_nametable_scan();

    // --- Deferred PartShowSuppress slot-hash scan ---
    //
    // IndexedStringA carries the `CD_Helm` / `CD_Upperbody` / ... part names PartShowSuppress keys on; the table is
    // populated by the engine during world load. Loading LT at cold-launch (before main menu finishes wiring) would
    // otherwise leave PartShowSuppress inert for the whole session because the synchronous scan at LT init would
    // observe a near-empty table. The deferred worker mirrors the nametable pattern: poll for world-ready, scan until
    // every expected slot hash is present, then call init_slot_hashes once to commit.

    void launch_deferred_slot_hash_scan() noexcept;
    void join_deferred_slot_hash_scan();

    // --- Targeted-apply redirect ---
    //
    // When the user has the editing dropdown pinned to a non-controlled character AND `flag_apply_to_editing` is on,
    // overlay-UI entry points (manual_apply, manual_apply_slot, manual_clear, picker changes, preset cycles) call this
    // to redirect the next scheduled apply onto the editing character's body. The worker consumes the idx once --
    // subsequent engine-triggered applies (hooks) fall through to the default controlled-body path. Pass 0 to clear a
    // pending redirect without scheduling.
    void set_targeted_apply_char_idx(std::uint32_t charIdx) noexcept;

    // Read the current pending redirect without consuming it. Used by entry points that want to skip scheduling
    // entirely when the editing character isn't live (the worker would only fall back to the controlled body, which is
    // the legacy cross-body behaviour the user explicitly opted out of).
    [[nodiscard]] std::uint32_t pending_targeted_apply_char_idx() noexcept;

} // namespace Transmog
