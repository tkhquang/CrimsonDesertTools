#ifndef CDCORE_CONTROLLED_CHAR_HPP
#define CDCORE_CONTROLLED_CHAR_HPP

#include <cstdint>
#include <string_view>

// ---------------------------------------------------------------------------
// Controlled-character resolver.
//
// Walks the live WorldSystem chain on every query and identifies the
// currently-controlled protagonist via a two-part decode: a structural
// invariant for Kliff plus a slot-index diff for the two companions.
//
//   *(WS holder)           -> WorldSystem instance (session heap)
//   *(ws   + 0x30)         -> ActorManager singleton
//   *(am   + 0x28)         -> UserActor singleton (ClientUserActor)
//   *(user + 0xD0)         -> primary actor slot (always Kliff in the
//                             current story progression)
//   *(user + 0xD8)         -> controlled client actor (rotates on swap)
//
// Decode rules:
//   1. If *(user + 0xD0) == *(user + 0xD8), the primary slot coincides with
//      the controlled slot, which only happens when Kliff is the active
//      character. This is a pure structural check, independent of any
//      numeric field that could shift across saves.
//
//   2. Otherwise, a companion is controlled. The byte at actor+0x60 is a
//      per-actor slot index assigned at allocation time in a fixed order
//      (Kliff, Damiane, Oongka). The absolute value varies per save (the
//      slot-index pool is shared with other actors), but WITHIN any given
//      save:
//          Damiane.slot == Kliff.slot + 1
//          Oongka.slot  == Kliff.slot + 2
//      Using Kliff's slot (always at user+0xD0) as the anchor, the diff
//      against the controlled actor's slot uniquely identifies the
//      companion.
//
// Previous decode attempts -- (party_class, char_kind) at +0xDC/+0xEC -- are
// NOT used. Both values shift with party composition (e.g. Damiane in a
// 3-party save and Oongka in the same save both read (party_class=3,
// char_kind=2), making the key structurally ambiguous).
//
// To absorb transient torn reads (the controlled-actor pointer can be
// briefly stale during a swap event) the resolver keeps a last-known-good
// cache: any decode that does not match one of the three known identities
// returns the cached value instead, and the cache refreshes whenever a
// fresh known-identity decode succeeds.
//
// The resolver requires the consumer to publish the WorldSystem holder
// static address once (typically right after that mod's own AOB scan
// resolves it). Until set_world_system_holder() has been called, every
// query returns Unknown. This avoids duplicating the WorldSystem AOB scan
// across every consumer.
//
// Known assumption: character allocation order is always Kliff, Damiane,
// Oongka. A save containing only Kliff + Oongka (no Damiane) would allocate
// Oongka at Kliff+1 and would be decoded as Damiane. No such save has been
// observed in the current game content; if one arises, the consumer mod's
// overlay allows manual override and the misdetection is a UX hiccup, not a
// correctness failure.
//
// Thread safety:
//   - set_world_system_holder() is safe from any thread; later writers win,
//     which is the intended behaviour if a consumer re-resolves on reload.
//   - current_controlled_character() is non-blocking and SEH-guarded; safe
//     to call from any thread including the rendering thread.
// ---------------------------------------------------------------------------

namespace CDCore
{
    /**
     * @brief Identifies one of the three controlled playable characters.
     * @details The Unknown sentinel is returned before
     *          set_world_system_holder() has been called, before the chain
     *          walk yields a known identity (very early in the loading
     *          screen), and after invalidate_controlled_character() until
     *          the next chain walk observes one of the three known keys
     *          and refreshes the cache.
     */
    enum class ControlledCharacter : std::uint8_t
    {
        Unknown,
        Kliff,
        Damiane,
        Oongka,
    };

    /**
     * @brief Publish the WorldSystem holder static address to the
     *        controlled-character resolver.
     * @details Both consumer mods (CrimsonDesertEquipHide and
     *          CrimsonDesertLiveTransmog) AOB-scan for the WorldSystem
     *          holder during their own init. Whichever resolves first
     *          should hand the address to Core via this call so the
     *          resolver does not need its own AOB scan. Subsequent calls
     *          overwrite the previous value, which is the intended
     *          behaviour when a consumer re-scans.
     * @param holderAddr Absolute address of the WorldSystem holder slot
     *                   (the static qword whose dereference yields the
     *                   live WorldSystem instance pointer). Pass 0 to
     *                   disable the resolver.
     */
    void set_world_system_holder(std::uintptr_t holderAddr) noexcept;

    /**
     * @brief Resolve the currently-controlled character.
     * @details Walks WorldSystem -> ActorManager -> UserActor, reads both
     *          the primary actor at user+0xD0 and the controlled actor at
     *          user+0xD8 plus the slot-index byte at +0x60 on each. Decodes
     *          Kliff by the structural invariant
     *          `*(user+0xD0) == *(user+0xD8)` and the two companions by
     *          the diff between the controlled slot and the primary slot
     *          (Damiane = primary + 1, Oongka = primary + 2). When the
     *          decode does not match a known identity (faulted chain walk,
     *          torn read across a swap transition, unrecognised future
     *          slot offset) the resolver returns the previously-cached
     *          identity instead, so transient drift does not flap the
     *          controlled character.
     * @return The identified character, or ControlledCharacter::Unknown
     *         when the WorldSystem holder has not been published yet,
     *         when the chain walk faults, or when neither the live decode
     *         nor the last-known-good cache yields a known value.
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
     * @brief Clear the last-known-good identity cache.
     * @details Call on world-reload transitions (save load, return to
     *          title) so the resolver does not keep returning the
     *          previous save's character while the new save is still
     *          populating the engine state. After invalidation the
     *          resolver returns Unknown until the next chain walk observes
     *          one of the three known keys and refreshes the cache. Safe
     *          to call from any thread; non-blocking.
     */
    void invalidate_controlled_character() noexcept;

} // namespace CDCore

#endif // CDCORE_CONTROLLED_CHAR_HPP
