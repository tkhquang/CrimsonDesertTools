#ifndef CDCORE_CONTROLLED_CHAR_HPP
#define CDCORE_CONTROLLED_CHAR_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

// ---------------------------------------------------------------------------
// Controlled-character resolver.
//
// Walks the live server-side party-state chain on every query and identifies
// the currently-controlled protagonist by which fixed-offset slot inside the
// party container has its active-flag byte set.
//
//   *(player_static)        -> root container
//   *(root  + 0x18)         -> pa::NwVirtualAsyncSession
//   *(nwSes + 0xA0)         -> pa::ServerUserActor
//   *(srvUA + 0xD0)         -> pa::ServerChildOnlyInGameActor
//                              (the party container)
//
// Inside the party container the three protagonist slots are inline structs
// at fixed offsets with stride 0x100:
//
//   party + 0x68            = Kliff slot
//   party + 0x168           = Damiane slot
//   party + 0x268           = Oongka slot
//
// Per slot the active-flag byte at slot+0x2C is the discriminator: exactly
// one slot reads 1 while the player is in-world (cutscenes / loading screens
// produce zero hot slots, which the resolver reports as Unknown). The +0x40
// dword is an observability handle that is volatile across sessions and
// asset rebuilds; it is logged for diagnostic correlation only and never
// participates in identity decoding.
//
// Why identity is decoded by SLOT OFFSET and not by a stored character ID:
// inside the party container the slot for a given protagonist is at a fixed
// compile-time-known offset, which is a structural invariant of the engine
// layout and cannot drift across actor-pool reuse, save-load reallocation,
// or radial-menu swaps. Earlier resolver iterations decoded a per-actor
// byte field that the engine repurposed across versions; the slot-offset
// approach is immune to that class of regression because there is no engine
// field whose meaning the resolver depends on.
//
// To absorb transient torn reads (the active-flag byte can momentarily
// disagree across the three slots during a swap event when the engine is
// still flipping bytes) the resolver keeps a last-known-good cache: any
// query that returns no single-1-bit-hot slot reuses the cached value
// instead, and the cache refreshes whenever a fresh single-bit-hot decode
// succeeds.
//
// The resolver requires the consumer to publish the player static address
// once (typically right after that mod's own AOB scan resolves it). Until
// set_player_static_holder() has been called, every query returns Unknown.
// This avoids duplicating the AOB scan across every consumer.
//
// Thread safety:
//   - set_player_static_holder() and set_world_system_holder() are safe from
//     any thread; later writers win, which is the intended behaviour if a
//     consumer re-resolves on reload.
//   - current_controlled_character() is non-blocking and SEH-guarded; safe
//     to call from any thread including the rendering thread.
// ---------------------------------------------------------------------------

namespace CDCore
{
    /**
     * @brief Identifies one of the three controlled playable characters.
     * @details The Unknown sentinel is returned before
     *          set_player_static_holder() has been called, before the chain
     *          walk yields a known identity (very early in the loading
     *          screen, or while a cutscene zeroes every slot's active flag),
     *          and after invalidate_controlled_character() until the next
     *          chain walk observes one of the three known keys and refreshes
     *          the cache.
     */
    enum class ControlledCharacter : std::uint8_t
    {
        Unknown,
        Kliff,
        Damiane,
        Oongka,
    };

    /**
     * @brief Publish the WorldSystem holder static address to Core.
     * @details Retained for callers that need the WorldSystem reach
     *          (UserActor, body-pool walks, mesh-bound state). The
     *          controlled-character resolver no longer consumes this
     *          pointer; identification is handled via
     *          set_player_static_holder() instead.
     * @param holderAddr Absolute address of the WorldSystem holder slot
     *                   (the static qword whose dereference yields the
     *                   live WorldSystem instance pointer). Pass 0 to
     *                   clear.
     */
    void set_world_system_holder(std::uintptr_t holderAddr) noexcept;

    /**
     * @brief Publish the server-side player static address to the
     *        controlled-character resolver.
     * @details Both consumer mods (CrimsonDesertEquipHide and
     *          CrimsonDesertLiveTransmog) AOB-scan for the player static
     *          during their own init. Whichever resolves first should hand
     *          the address to Core via this call so the resolver does not
     *          need its own AOB scan. Subsequent calls overwrite the
     *          previous value.
     * @param holderAddr Absolute address of the player static slot (the
     *                   static qword whose dereference yields the root
     *                   container described in the file-level comment).
     *                   Pass 0 to disable the resolver.
     */
    void set_player_static_holder(std::uintptr_t holderAddr) noexcept;

    /**
     * @brief Resolve the currently-controlled character.
     * @details Walks the server-side party-state chain and reads the
     *          active-flag byte at slot+0x2C for each of the three fixed
     *          protagonist offsets (Kliff=0x68, Damiane=0x168,
     *          Oongka=0x268). Returns the slot whose flag reads 1. When no
     *          slot is hot (cutscene, loading screen) or more than one is
     *          hot (transient torn read mid-swap), the resolver returns the
     *          previously-cached identity instead, so transient drift does
     *          not flap the controlled character.
     * @return The identified character, or ControlledCharacter::Unknown
     *         when the player static has not been published yet, when the
     *         chain walk faults, or when neither the live decode nor the
     *         last-known-good cache yields a known value.
     */
    [[nodiscard]] ControlledCharacter current_controlled_character() noexcept;

    /**
     * @brief Short display name for a controlled character.
     * @return A static string view ("Kliff" / "Damiane" / "Oongka") or
     *         an empty view for Unknown.
     */
    [[nodiscard]] std::string_view controlled_character_name(
        ControlledCharacter ch) noexcept;

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
     *          the body has not yet been seen as controlled (the user
     *          has never been that character in this session) --
     *          callers should treat 0 as "unknown" and fall back to
     *          the active character's mask, which is the safe
     *          degradation.
     *
     *          Why a learning cache instead of a structural decode:
     *          the v1.04.00 client body has no reachable back-reference
     *          to its protagonist slot in the party container, and the
     *          per-actor +0x50 byte that earlier builds used as a slot
     *          index ceased to discriminate on this build. Observing
     *          the controlled-body / known-character pairing each
     *          resolver tick is the only stable mapping available from
     *          the public game-state surface.
     *
     *          Cache scope: per-DLL (CDCore is statically linked into
     *          each consumer Logic DLL). Each consumer learns
     *          independently. Cleared by
     *          invalidate_controlled_character() on save-load so dead
     *          body pointers cannot mis-attribute a future protagonist.
     *          Preserved across in-session radial swaps (callers should
     *          use invalidate_swap_caches() on those transitions) since
     *          the engine rotates user+0xD8 between existing body
     *          pointers in the pool without reallocating bodies.
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
     * @brief Install the radial-swap-key capture mid-hook at the
     *        resolved address.
     * @details The hook target must be the instruction immediately
     *          following the EAX-load from the radial-UI input pointer
     *          (the `mov [rbp+0x78], eax` at sub_1422019A0+e9 in
     *          v1.04.00). At hook entry the low 32 bits of RAX hold the
     *          requested characterinfo key (1 = Kliff, 4 = Damiane,
     *          6 = Oongka). The hook stamps a short-lived process-wide
     *          pending-key record so the next chain-walk that observes
     *          a non-Kliff identity can attribute the user-actor pointer
     *          to the captured key, building an actor->character cache
     *          that the resolver consults before falling back to the
     *          slot-offset decode.
     *
     *          Single-owner semantics within a DLL:
     *          - First call with a non-zero @p hookAddr: builds the
     *            SafetyHook MidHook against that address.
     *          - Subsequent calls with the SAME @p hookAddr: idempotent
     *            no-op (the hook is already live at the requested site).
     *          - Subsequent calls with a DIFFERENT non-zero @p hookAddr:
     *            destroy the existing MidHook and rebuild against the
     *            new address (used if an AOB re-resolution shifts the
     *            target mid-session).
     *          - Call with @p hookAddr == 0: tear down the MidHook so
     *            the patched bytes return to the original instruction
     *            sequence. Safe to call when no hook is installed.
     *
     *          Cross-DLL coordination: CrimsonDesertCore is linked as a
     *          STATIC library, so each consumer Logic DLL (LiveTransmog,
     *          EquipHide) carries its own copy of CDCore state and its
     *          own SafetyHook MidHook against the shared process address.
     *          When both consumers call this function against the same
     *          target site, SafetyHook's internal chaining lets the two
     *          MidHooks coexist transparently -- each consumer's
     *          trampoline runs independently on every radial swap. The
     *          AOB cascade in cdcore/anchors.hpp k_radialSwapKeyCandidates
     *          is ordered so the post-patch-tolerant patterns resolve
     *          first, ensuring the second consumer to scan still finds
     *          the same hook target the first consumer already patched.
     *
     *          Failure modes (returns false): SafetyHook allocation
     *          failure (ENOMEM-shaped, rare), trampoline placement
     *          failure (target page already heavily hooked), or address
     *          below the 64 KiB guard region. On any failure the
     *          resolver continues to function via the slot-offset decode
     *          and the LKG cache; only the deterministic
     *          radial-attribution layer is missing for this consumer.
     *
     * @param hookAddr Absolute address of the `mov [rbp+0x78], eax`
     *                 instruction inside sub_1422019A0. The AOB cascade
     *                 in cdcore/anchors.hpp k_radialSwapKeyCandidates
     *                 resolves to this address. Pass 0 to tear down.
     * @return true on successful install / idempotent no-op / teardown,
     *         false on SafetyHook error.
     */
    bool install_radial_swap_hook(std::uintptr_t hookAddr) noexcept;

    /**
     * @brief Tear down the radial-swap-key mid-hook owned by this DLL.
     * @details Equivalent to install_radial_swap_hook(0). Safe to call
     *          when no hook is installed; safe to call from shutdown
     *          paths. Idempotent. Each consumer DLL owns its own
     *          MidHook; calling this in one DLL does not affect any
     *          other DLL's MidHook against the same process address.
     */
    void uninstall_radial_swap_hook() noexcept;

    /**
     * @brief Full world-reload invalidation. Clears the last-known-good
     *        identity cache, the actor->character cache, the pending
     *        radial-swap-key record, AND the body->character learning
     *        cache.
     * @details Call on world-reload transitions (save load, return to
     *          title) so the resolver does not keep returning the
     *          previous save's character while the new save is still
     *          populating the engine state. After invalidation the
     *          resolver returns Unknown until the next chain walk observes
     *          one of the three known keys and refreshes the cache.
     *
     *          Why the body cache is flushed here: save-load reallocates
     *          the body pool, so any cached body pointer becomes a
     *          dangling key the engine may reuse for a different
     *          protagonist. Mis-attribution after reload is prevented by
     *          dropping the entire body map.
     *
     *          For in-session protagonist swaps (radial menu rotation,
     *          scripted Kliff returns) use invalidate_swap_caches()
     *          instead -- it preserves the body cache because radial
     *          swaps only rotate user+0xD8 between EXISTING body
     *          pointers in the pool, leaving previously-learned
     *          bodies still valid.
     *
     *          Safe to call from any thread; non-blocking.
     */
    void invalidate_controlled_character() noexcept;

    /**
     * @brief Clear the LKG identity cache, the actor->character cache,
     *        and the pending radial-swap-key record, but preserve the
     *        body->character learning cache.
     * @details Use this on in-session protagonist swaps (radial menu
     *          rotation, scripted Kliff returns). The controlled actor
     *          pointer at user+0xD8 rotates between existing body
     *          pointers in the pool; the body pointers themselves stay
     *          valid for the lifetime of the session, so previously-
     *          learned body identities remain correct after the swap.
     *
     *          What this clears:
     *            - last-known-good cached identity (s_lastGoodChar)
     *            - actor (userActor parent) -> character cache
     *            - pending radial-swap-key record
     *
     *          What this preserves:
     *            - body (user+0xD8 pointer) -> character cache
     *
     *          For full world-reload semantics use
     *          invalidate_controlled_character() instead, which also
     *          flushes the body cache because save-load may reuse old
     *          body pointers for different characters.
     *
     *          Safe to call from any thread; non-blocking.
     */
    void invalidate_swap_caches() noexcept;

} // namespace CDCore

#endif // CDCORE_CONTROLLED_CHAR_HPP
