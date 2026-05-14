#ifndef CDCORE_CONTROLLED_CHAR_HPP
#define CDCORE_CONTROLLED_CHAR_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

// ---------------------------------------------------------------------------
// Controlled-character resolver -- focus-broadcast architecture.
//
// Identity is decoded from the engine's own focus-actor broadcast event.
// The engine assigns each protagonist a stable u32 hash handle at static
// init (via "focus-actor-kliff/oongka/damian" string registration) and
// fires `sub_14353BA60` on every focus mutation -- cold-load,
// save-load reattach, radial swap. R9 at function entry carries the
// new focus handle; the CDCore MidHook compares against the three
// AOB-resolved hash globals and stamps a process-wide cache.
//
// Init flow (consumer is LiveTransmog or EquipHide):
//   1. resolve_and_publish_focus_actor_globals()  -> publishes the 3
//      hash global addresses via set_focus_actor_hash_globals().
//   2. install_focus_broadcast_hook(addr)         -> hooks
//      sub_14353BA60 entry. Callback writes the resolved character
//      into the Tier-0 cache.
//   3. install_radial_swap_hook(addr)             -> hooks
//      sub_141B04040 entry. Callback timestamps a flag consumed
//      by `radial_swap_pending()` (used by EquipHide to
//      disambiguate radial swap from save-load arena rotation).
//
// Body cache. As a side effect of every successful
// current_controlled_character() resolve, the resolver stamps
// (user+0xD8 -> character) into a learning cache consumed by
// resolve_character_idx_for_body() and snapshot_body_cache(). The
// cache reaches user+0xD8 via the WorldSystem chain published by
// set_world_system_holder().
//
// Thread safety:
//   - All setters (set_world_system_holder, set_focus_actor_hash_
//     globals) are safe from any thread; later writers win.
//   - current_controlled_character() is non-blocking and SEH-
//     guarded; safe from any thread including the rendering thread.
// ---------------------------------------------------------------------------

namespace CDCore
{
    /**
     * @brief Identifies one of the three controlled playable characters.
     * @details The Unknown sentinel is returned before
     *          set_focus_actor_hash_globals() has been called, before the
     *          engine has fired its first focus-actor broadcast for the
     *          session, and after invalidate_controlled_character() until
     *          the next broadcast or structural Kliff body anchor
     *          repopulates the cache.
     */
    enum class ControlledCharacter : std::uint8_t
    {
        Unknown,
        Kliff,
        Damiane,
        Oongka,
    };

    /**
     * @brief Resolve the currently-controlled character.
     * @details Reads the Tier-0 focus-broadcast cache; falls back to
     *          the LKG cache when the broadcast has not yet fired
     *          this session. Side-effect: stamps (user+0xD8 ->
     *          character) into the body learning cache on every
     *          successful resolve.
     * @return The identified character, or
     *         ControlledCharacter::Unknown when neither Tier-0 nor
     *         the LKG cache holds a known value (the engine has not
     *         yet broadcast a focus-actor change this session, or
     *         caches were just invalidated).
     */
    [[nodiscard]] ControlledCharacter current_controlled_character() noexcept;

    /**
     * @brief Short display name for a controlled character.
     */
    [[nodiscard]] std::string_view controlled_character_name(
        ControlledCharacter ch) noexcept;

    /**
     * @brief Publish the WorldSystem holder static address to Core.
     * @details Used by walk_to_controlled_body_seh() to reach the
     *          rotating user+0xD8 client body pointer that the body
     *          learning cache keys on.
     * @param holderAddr Absolute address of the WorldSystem holder slot
     *                   (the static qword whose dereference yields the
     *                   live WorldSystem instance pointer). Pass 0 to
     *                   clear.
     */
    void set_world_system_holder(std::uintptr_t holderAddr) noexcept;

    /**
     * @brief Convenience wrapper that returns the live name.
     * @return Short name of the controlled character, or an empty view if
     *         unknown.
     */
    [[nodiscard]] std::string_view current_controlled_character_name() noexcept;

    /**
     * @brief One-based index of the controlled character.
     * @return 1 for Kliff, 2 for Damiane, 3 for Oongka, or 0 for unknown.
     *         The zero sentinel matches the game's own pre-world value so
     *         consumers can treat it uniformly.
     */
    [[nodiscard]] std::uint32_t current_controlled_character_idx() noexcept;

    /**
     * @brief One-based protagonist index for an arbitrary client body
     *        pointer.
     * @details Looks up @p body in the learning cache populated by
     *          current_controlled_character() each time the resolver
     *          observes a known controlled identity. Returns 0 when
     *          the body has not yet been seen as controlled this
     *          session -- callers should treat 0 as "unknown" and
     *          fall back to the active character's mask.
     *
     *          Cache scope: per-DLL (CDCore is statically linked into
     *          each consumer Logic DLL). Each consumer learns
     *          independently. Cleared by
     *          invalidate_controlled_character() on save-load.
     *          Preserved across in-session radial swaps (callers
     *          should use invalidate_swap_caches() on those
     *          transitions) since the engine rotates user+0xD8
     *          between existing body pointers in the pool without
     *          reallocating bodies.
     *
     * @param body Pointer to a ClientChildOnlyInGameActor instance.
     * @return 1 for Kliff, 2 for Damiane, 3 for Oongka, or 0 for
     *         unknown (not yet learned, or invalid pointer).
     */
    [[nodiscard]] std::uint32_t resolve_character_idx_for_body(
        std::uintptr_t body) noexcept;

    /**
     * @brief One entry in the body learning cache snapshot.
     * @details charIdx uses the same 1-based encoding as
     *          current_controlled_character_idx(): 1 for Kliff, 2 for
     *          Damiane, 3 for Oongka. body is the user+0xD8 pointer
     *          observed at the time the entry was stamped.
     */
    struct BodyCacheEntry
    {
        std::uintptr_t body;
        std::uint32_t  charIdx;
    };

    /**
     * @brief Copy up to @p cap entries from the body learning cache into
     *        the caller-provided buffer.
     * @details Fills entries in unspecified iteration order. Returns the
     *          number of entries copied (capped at the smaller of @p cap
     *          and the live cache size). Acquires a shared lock on the
     *          cache mutex for the duration of the copy; the call is
     *          non-blocking from a contention standpoint because the
     *          cache writers are infrequent (one stamp per
     *          controlled-character resolution).
     *
     *          Entries with character == Unknown are skipped (they
     *          cannot occur in practice because current_controlled
     *          _character() only stamps known identities, but the guard
     *          is cheap).
     *
     * @param out Pointer to a buffer of at least @p cap entries.
     * @param cap Capacity of the buffer in entries.
     * @return Number of entries written.
     */
    [[nodiscard]] std::size_t snapshot_body_cache(
        BodyCacheEntry *out,
        std::size_t cap) noexcept;

    /**
     * @brief Publish the three engine focus-actor hash globals to CDCore.
     * @details The engine assigns each protagonist (Kliff/Damiane/Oongka)
     *          a per-process u32 handle at static init. The three globals
     *          live at fixed addresses inside the binary's writable data
     *          section; consumers AOB-resolve those addresses (see
     *          `CDCore::resolve_and_publish_focus_actor_globals()`) and
     *          publish them here. The values themselves -- which differ
     *          per game version -- are read from these addresses every
     *          query in the focus-broadcast MidHook callback, so we
     *          never hardcode the numeric handles.
     *
     *          When all three addresses are non-zero the new Tier-0
     *          decoder in `current_controlled_character()` activates and
     *          returns whichever character last fired the focus-actor
     *          broadcast. Pass any address as zero to disable the layer
     *          (the resolver falls through to the existing slot-offset
     *          decode and the LKG cache).
     *
     *          Per-DLL state (CDCore is statically linked into each
     *          consumer): each consumer publishes independently. A
     *          consumer that fails to AOB-resolve still receives the
     *          slot-offset / LKG behaviour as before.
     *
     *          Safe to call from any thread; later writers win.
     *
     * @param kliffAddr Absolute address of the Kliff hash u32 global.
     * @param oongkaAddr Absolute address of the Oongka hash u32 global.
     * @param damianAddr Absolute address of the Damian hash u32 global.
     */
    void set_focus_actor_hash_globals(std::uintptr_t kliffAddr,
                                      std::uintptr_t oongkaAddr,
                                      std::uintptr_t damianAddr) noexcept;

    /**
     * @brief One-shot helper that AOB-resolves the three focus-actor
     *        hash globals and publishes them via
     *        `set_focus_actor_hash_globals()`.
     * @details Scans .text for the three identical bridge functions
     *          described in `CDCore::Anchors::k_focusActorInitPattern`,
     *          dereferences each match's RIP-relative string and dword
     *          LEAs, identifies which protagonist each bridge serves
     *          by matching the string against
     *          "focus-actor-kliff/oongka/damian", then publishes the
     *          three resolved dword addresses to CDCore. Returns true
     *          on full success (all three protagonists resolved). On
     *          any failure (pattern mismatch, fewer than 3 hits, name
     *          dispatch ambiguity) returns false WITHOUT calling the
     *          setter, leaving the Tier-0 layer disabled.
     *
     *          Idempotent: safe to call multiple times. Each successful
     *          call overwrites the previously published addresses (no-op
     *          if the addresses are unchanged across calls).
     *
     *          Logs a single Info line on success and a Warning on
     *          failure.
     */
    [[nodiscard]] bool resolve_and_publish_focus_actor_globals() noexcept;

    /**
     * @brief Install the focus-actor broadcast capture mid-hook at the
     *        resolved address (sub_14353BA60 entry).
     * @details The hook fires on every focus-actor list mutation
     *          (subscription event), reads `ctx.r9` as the new focus
     *          handle, and -- when r9 matches one of the three globals
     *          published via `set_focus_actor_hash_globals()` --
     *          stamps the resolved character into a process-wide cache
     *          consumed by `current_controlled_character()`'s Tier-0
     *          layer.
     *
     *          The hook is the canonical signal for cold-load and
     *          save-load reattach because the engine fires the
     *          broadcast during world-spawn before the player can
     *          interact, populating the cache before any mod query
     *          occurs. Radial swaps trigger it the same way.
     *
     *          ~99.8% of the broadcast firings carry NPC focus handles
     *          in r9 (filtered out by the equality check against the
     *          three published character globals), so the per-call
     *          overhead is three relaxed atomic loads and three
     *          dword compares.
     *
     *          Single-owner-per-DLL semantics mirror
     *          `install_radial_swap_hook()`. Pass `hookAddr == 0` to
     *          tear down. Failure modes match the radial-swap path.
     *
     * @param hookAddr Absolute address of `sub_14353BA60` entry.
     *                 The AOB cascade in
     *                 `cdcore/anchors.hpp::k_focusBroadcastCandidates`
     *                 resolves to this address. Pass 0 to tear down.
     * @return true on successful install / idempotent no-op / teardown,
     *         false on SafetyHook error.
     */
    bool install_focus_broadcast_hook(std::uintptr_t hookAddr) noexcept;

    /**
     * @brief Tear down the focus-broadcast mid-hook owned by this DLL.
     * @details Equivalent to `install_focus_broadcast_hook(0)`. Safe
     *          when no hook is installed; safe in shutdown paths.
     *          Idempotent. Each consumer DLL owns its own MidHook;
     *          calling this in one DLL does not affect any other
     *          DLL's MidHook against the same process address.
     */
    void uninstall_focus_broadcast_hook() noexcept;

    /**
     * @brief Install the radial-swap-input mid-hook at the resolved
     *        address (sub_141B04040 entry).
     * @details Post-Tier-0 refactor the hook callback is a single
     *          atomic store: it stamps a monotonic timestamp consumed
     *          by `radial_swap_pending()`. EquipHide reads that flag
     *          to disambiguate a user radial swap from a save-load
     *          arena rotation (both rotate user+0xD8 between bodies
     *          in different ways). The character identity itself
     *          comes from the focus-broadcast hook, not this one.
     *
     *          Single-owner-per-DLL semantics, cross-DLL chaining,
     *          and tear-down semantics mirror
     *          `install_focus_broadcast_hook()`. Pass `hookAddr == 0`
     *          to tear down. Failure modes: SafetyHook allocation /
     *          trampoline placement failure, or address below the
     *          64 KiB guard region. On any failure
     *          `radial_swap_pending()` always returns false (no
     *          stamping occurs) and EquipHide falls back to the
     *          conservative save-load path -- still correct, slightly
     *          more aggressive on body-cache wipes.
     *
     * @param hookAddr Absolute address of `sub_141B04040` entry
     *                 (the AOB cascade in
     *                 `cdcore/anchors.hpp::k_radialSwapKeyCandidates`
     *                 resolves to this address). Pass 0 to tear down.
     * @return true on successful install / idempotent no-op / teardown,
     *         false on SafetyHook error.
     */
    bool install_radial_swap_hook(std::uintptr_t hookAddr) noexcept;

    /**
     * @brief Tear down the radial-swap mid-hook owned by this DLL.
     * @details Equivalent to install_radial_swap_hook(0). Idempotent
     *          and shutdown-safe.
     */
    void uninstall_radial_swap_hook() noexcept;

    /**
     * @brief Whether a radial-swap input was observed within the
     *        pending-key window.
     * @details Returns true when the radial-swap mid-hook has fired
     *          within the pending window (currently 2 s). Lets
     *          consumers disambiguate an in-session protagonist swap
     *          from a save-load when both appear as a controlled-
     *          actor pointer rotation: a swap fired by user input
     *          has a fresh timestamp, a save-load does not.
     *
     *          Per-DLL state (CDCore is statically linked into each
     *          consumer): observes the timestamp written by THIS
     *          DLL's radial-swap callback.
     *
     *          Safe to call from any thread; non-blocking (one
     *          relaxed atomic load + a tick comparison).
     */
    [[nodiscard]] bool radial_swap_pending() noexcept;

    /**
     * @brief Full world-reload invalidation. Clears Tier-0, LKG,
     *        radial-swap timestamp, AND the body learning cache.
     * @details Call on world-reload transitions (save load, return to
     *          title) so the resolver does not keep returning the
     *          previous save's character while the new save is still
     *          populating the engine state. After invalidation the
     *          resolver returns Unknown until the next focus-actor
     *          broadcast fires.
     *
     *          The body cache is flushed because save-load
     *          reallocates the body pool; cached body pointers may be
     *          reused for a different protagonist.
     *
     *          For in-session protagonist swaps (radial menu) use
     *          invalidate_swap_caches() instead -- preserves the body
     *          cache because radial swaps only rotate user+0xD8
     *          between EXISTING body pointers in the pool.
     *
     *          Safe to call from any thread; non-blocking.
     */
    void invalidate_controlled_character() noexcept;

    /**
     * @brief Clear Tier-0 cache, LKG, and the radial-swap timestamp;
     *        preserve the body learning cache.
     * @details Use this on in-session protagonist swaps. The
     *          controlled actor pointer at user+0xD8 rotates between
     *          existing body pointers in the pool; the body pointers
     *          themselves stay valid for the lifetime of the session.
     *
     *          For full world-reload semantics use
     *          invalidate_controlled_character() which also flushes
     *          the body cache.
     *
     *          Safe to call from any thread; non-blocking.
     */
    void invalidate_swap_caches() noexcept;

} // namespace CDCore

#endif // CDCORE_CONTROLLED_CHAR_HPP
