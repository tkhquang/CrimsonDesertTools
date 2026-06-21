#ifndef CDCORE_CONTROLLED_CHAR_HPP
#define CDCORE_CONTROLLED_CHAR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

// ---------------------------------------------------------------------------
// Controlled-character resolver -- static-chain + body-mesh asset-path
// architecture.
//
// Identity is derived from a deterministic static-chain walk plus a
// body-mesh asset-path read on the CCOIA
// (pa::ClientChildOnlyInGameActor):
//
//   [playerBase]                        ; AOB-resolved module-static slot
//      -> +0x58  = pa::ClientUserActor
//          -> +0x08  = CCOIA sub-manager
//              -> +0x30 = Kliff CCOIA           (always-present anchor)
//              -> +0x38 = currently-controlled CCOIA
//          -> +0x78 = vec<ActorComponent*> data ptr
//              -> vec[2] = pa::ClientChildContainerActorComponent
//                  -> +0x18 = 100-entry actor list (16-byte stride ptr+flag)
//
//   ClientActorManager+0x130 -> CCOIA-only actor array (8-byte stride,
//   element capacity at +0x13C):
//     [0] = Kliff       (always present)
//     [1] = Damiane     (when loaded)
//     [2] = Oongka      (when loaded)
//     [3+] = humanoid NPCs / followers
//
// CCOIA classification via appearance-config asset path
// (cross-session stable, character-codename specific):
//
//   CCOIA + 0x68 -> +0x40 -> +0x40 -> +0x38 = std::string
//
//   Kliff   -> "character/appearance/1_pc/1_phm/cd_phm_macduff/cd_phm_macduff_00000.app_xml"
//   Damiane -> "character/appearance/1_pc/2_phw/cd_phw_damian/cd_phw_damian_00000.app_xml"
//   Oongka  -> "character/appearance/1_pc/1_phm/cd_phm_oongka/cd_phm_oongka_00000.app_xml"
//
//   The path embeds the character's internal codename twice (subfolder
//   + filename): "macduff" = Kliff, "damian" = Damiane, "oongka" =
//   Oongka. The codename is character-specific (not a skeleton
//   archetype like phw), so a future protagonist that shares Damiane's
//   female-warrior skeleton would still get a unique codename and
//   classify as Unknown rather than be mis-identified as Damiane.
//
//   The appearance config is bound at actor spawn and does NOT change
//   with outfit, animation, or save-load (only the heap addresses
//   rotate). NPCs and follower humanoids don't carry an appearance
//   config at this path -- classify returns Unknown for them.
//
// Init flow:
//   From any thread, call current_controlled_character() etc. -- the
//   resolver walks the chain on every call (SEH-guarded). No hooks,
//   no learning caches, no broadcast subscriptions. The player-base
//   slot is AOB-resolved lazily on first use via
//   CDCore::Anchors::k_clientActorManagerGlobalCandidates (3-tier
//   cascade across distinct instructions in the publishing function).
//
// Thread safety:
//   - All functions are non-blocking and SEH-guarded; safe from any thread
//     including the rendering thread.
//   - The only mutable internal state is the world-generation counter
//     plus the Kliff-CCOIA tracker that drives it, both atomics.
// ---------------------------------------------------------------------------

namespace CDCore
{
    /**
     * @brief pa::ClientActorManager / pa::ClientUserActor memory-layout
     *        offsets shared by every controlled-character resolver in
     *        the suite.
     * @details Single authority for the offsets that describe the
     *          controlled-character chain so a game-update struct drift
     *          is a one-line edit here instead of N edits scattered
     *          across the mods. Three resolvers consume these:
     *            - CDCore controlled_char.cpp -- roots at the published
     *              ClientActorManagerGlobal slot (anchors.hpp AOB).
     *            - LiveTransmog transmog_worker.cpp
     *              (resolve_player_component / read_*_actor_ptr_seh) and
     *            - EquipHide background_threads.cpp (controlled-actor
     *              poll) -- both root at the WorldSystem holder
     *              (k_worldSystemToActorManager) instead.
     *          The two roots are independent anchors that resolve to
     *          the SAME pa::ClientActorManager, so they share
     *          k_actorManagerToUserActor downstream. The WorldSystem-
     *          rooted walks then read k_userActorToControlled directly,
     *          reaching the same controlled CCOIA the CDCore chain
     *          resolves indirectly through the sub-manager (+0x38).
     *
     *          A game update that re-lays-out pa::ClientActorManager
     *          moves k_actorManagerToUserActor; it is the offset most
     *          likely to change on a patch, which is why it lives here
     *          once rather than inline at each call site. See
     *          CrimsonDesertCore/include/cdcore/anchors.hpp for the AOB
     *          that resolves the manager itself.
     */
    namespace ActorChainOffsets
    {
        /// Offset from pa::ClientActorManager to pa::ClientUserActor.
        inline constexpr std::ptrdiff_t k_actorManagerToUserActor = 0x58;
        /// Offset from the WorldSystem holder qword to pa::ClientActorManager.
        inline constexpr std::ptrdiff_t k_worldSystemToActorManager = 0x30;
        /// Offset from pa::ClientUserActor to the controlled CCOIA.
        inline constexpr std::ptrdiff_t k_userActorToControlled = 0xD8;
    } // namespace ActorChainOffsets

    /**
     * @brief INI-tunable search radius (bytes, per side) for the rtti_dissect
     *        self-heal that recovers the manager->userActor offset after a patch
     *        shifts the struct layout.
     * @details Returns the atomic a consumer binds with
     *          DMK::Config::register_atomic("Advanced", "SelfHealWindow", ...).
     *          The heal reads it once per attempt (not a hot path) and clamps it
     *          to DMK::Rtti::MAX_HEAL_WINDOW; a non-positive value falls back to
     *          the built-in default. A wider window reaches a larger insertion
     *          before the userActor slot; this is a single independent landmark,
     *          but the manager references pa::ClientUserActor exactly once (the
     *          next pointer to it is megabytes away), so the 0x200 default is
     *          decoy-free across the whole heal range. Raise it via the INI only
     *          if a real patch shifts the field further.
     */
    [[nodiscard]] std::atomic<int> &heal_window_setting() noexcept;

    /**
     * @brief Identifies one of the three controlled playable characters.
     * @details Unknown is returned when the static chain has not yet
     *          been populated (cold-load), when it is mid-teardown
     *          (save-load window), or when a torn read faults out.
     */
    enum class ControlledCharacter : std::uint8_t
    {
        Unknown,
        Kliff,
        Damiane,
        Oongka,
    };

    /**
     * @brief CCOIA pointer of the currently-controlled character.
     * @details Walks [playerBase] -> +0x58 -> +0x08 -> +0x38. SEH-
     *          guarded. Returns 0 when the chain is mid-teardown (engine
     *          published a null along the path during save-load) or
     *          before the engine has finished cold-load wiring.
     */
    [[nodiscard]] std::uintptr_t current_controlled_ccoia() noexcept;

    /**
     * @brief Resolve the currently-controlled character.
     * @details Walks the static chain to obtain the controlled CCOIA,
     *          then classifies it by reading the appearance-config
     *          asset path at `CCOIA + 0x68 -> +0x40 -> +0x40 -> +0x38`
     *          and matching the embedded character codename
     *          (`cd_phm_macduff` / `cd_phw_damian` / `cd_phm_oongka`).
     *
     *          Falls back to the `+0x63` high-byte fast-path for
     *          Kliff when the appearance chain is mid-teardown (an
     *          NPC-controlled cutscene frame or save-load window
     *          where component pointers transiently null). Returns
     *          Unknown when neither path produces a match.
     */
    [[nodiscard]] ControlledCharacter current_controlled_character() noexcept;

    /**
     * @brief Short display name for a controlled character.
     */
    [[nodiscard]] std::string_view controlled_character_name(
        ControlledCharacter ch) noexcept;

    /**
     * @brief Inverse of controlled_character_name: resolve a short
     *        codename ("Kliff" / "Damiane" / "Oongka") back to its enum.
     * @return Unknown for any other string.
     */
    [[nodiscard]] ControlledCharacter character_from_name(
        std::string_view name) noexcept;

    /**
     * @brief One-based index for a character codename.
     * @return 1 Kliff, 2 Damiane, 3 Oongka, 0 otherwise. Matches the
     *         sentinel of current_controlled_character_idx() so name- and
     *         live-derived indices are interchangeable.
     */
    [[nodiscard]] std::uint32_t character_idx_from_name(
        std::string_view name) noexcept;

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
     * @brief Resolve the equip-slot (LT's a1) for an arbitrary CCOIA.
     * @details Walks `*(ccoia + 0x68) + 0x38`, which the engine pins
     *          to `pa::ClientEquipSlotActorComponent`. SEH-guarded;
     *          returns 0 on any null/fault or on inactive bodies where
     *          the engine zeroes the component slot.
     *
     *          Useful for iterating non-controlled player CCOIAs and
     *          dispatching engine APIs that take an equip-slot a1
     *          (e.g., LiveTransmog's `apply_all_transmog`).
     *
     * @param ccoia Pointer to a pa::ClientChildOnlyInGameActor instance.
     * @return The equip-slot pointer, or 0 on failure.
     */
    [[nodiscard]] std::uintptr_t equip_slot_for_ccoia(
        std::uintptr_t ccoia) noexcept;

    /**
     * @brief Classify an appearance-config asset path into a
     *        protagonist index using the codename set shared with the
     *        CCOIA classifier.
     * @details Locates the `/cd_` anchor in @p path and substring-
     *          matches the suffix against the three configured
     *          protagonist codenames (settable via
     *          @ref set_protagonist_codenames). Returns the same
     *          1-based indices @ref current_controlled_character_idx
     *          uses: 1 for Kliff, 2 for Damiane, 3 for Oongka. Returns
     *          0 for paths that don't contain a known codename (NPCs,
     *          monsters, empty paths).
     *
     *          Exposed so callers that already hold an actor's
     *          appearance / asset string (read from any source, not
     *          necessarily the CCOIA chain) can classify it without
     *          having to re-derive the chain walk used by
     *          @ref current_controlled_character_idx.
     *
     * @param path ASCII appearance path or asset string.
     * @return 1, 2, or 3 on a protagonist match; 0 otherwise.
     */
    [[nodiscard]] std::uint32_t classify_appearance_by_path(
        std::string_view path) noexcept;

    /**
     * @brief One entry in a live player-CCOIA snapshot.
     * @details charIdx uses the same 1-based encoding as
     *          current_controlled_character_idx(): 1 for Kliff, 2 for
     *          Damiane, 3 for Oongka. body is the CCOIA pointer.
     */
    struct BodyCacheEntry
    {
        std::uintptr_t body;
        std::uint32_t  charIdx;
    };

    /**
     * @brief Snapshot the live player CCOIAs into the caller-provided
     *        buffer.
     * @details Walks the static chain to enumerate the 1 to 3 player
     *          CCOIAs currently live in the world:
     *            - Kliff is always present (sub-manager +0x30).
     *            - Damiane / Oongka are pulled from the
     *              `ClientActorManager + 0x130` CCOIA-only actor array
     *              (8-byte stride) and accepted only when the
     *              appearance-config classifier matches their
     *              character codename. NPCs and follower humanoids
     *              share the array but fail the codename match, so
     *              the snapshot returns at most one Damiane and one
     *              Oongka entry.
     *
     *          The walk is SEH-guarded; on any fault returns 0. Kliff
     *          is always written first; remaining entries are in
     *          array-traversal order. Cap clamps the output.
     *
     * @param out Pointer to a buffer of at least @p cap entries.
     * @param cap Capacity of the buffer in entries.
     * @return Number of entries written (0 when the chain is
     *         unreachable, otherwise 1 to 3).
     */
    [[nodiscard]] std::size_t snapshot_body_cache(
        BodyCacheEntry *out,
        std::size_t cap) noexcept;

    /**
     * @brief Monotonic world-generation counter.
     * @details Incremented every time the resolver observes a new
     *          Kliff CCOIA pointer at sub-manager +0x30. The CCOIA
     *          sub-manager pointer itself is persistent across
     *          save-load (its address never changes within a process
     *          lifetime), so it cannot be used as the world-rebuild
     *          signal; Kliff's CCOIA IS reallocated on every save-
     *          load and is the correct trigger. Consumers cache the
     *          previous value and treat any change as "world has
     *          been rebuilt, flush local per-world state".
     *
     *          Returns the same value as long as the chain is
     *          reachable and the Kliff CCOIA pointer is unchanged.
     *          Returns the last-observed value when the chain is
     *          mid-teardown (does not regress on transient nulls).
     *
     *          A true save-load is the only event that bumps the
     *          counter, so consumers can distinguish in-session
     *          radial swaps (unchanged generation) from world-
     *          rebuilds (incremented generation) without a debounce
     *          window.
     */
    [[nodiscard]] std::uint64_t world_generation() noexcept;

    /**
     * @brief Diagnostic snapshot of the ChildContainer actor list.
     * @details Walks the same actor list snapshot_body_cache uses but
     *          returns the raw identity bytes for every live entry,
     *          without filtering by character classifier. Use to
     *          diagnose snapshot misses (e.g., when a protagonist's
     *          +0x60 load-order delta does not match the expected
     *          Kliff_low+1 / +2 pattern).
     *
     *          Cheap (one chain walk + one list walk). SEH-guarded.
     */
    struct ActorListDebugEntry
    {
        std::uintptr_t ccoia;
        std::uint64_t  flag;     // raw 8-byte flag at entry+8
        std::uint32_t  identity; // packed dword at CCOIA+0x60
    };

    struct ActorListDebugSummary
    {
        std::uintptr_t mgr           = 0;
        std::uintptr_t userActor     = 0;
        std::uintptr_t subMgr        = 0;
        std::uintptr_t kliffCcoia    = 0;
        std::uintptr_t controlled    = 0;
        std::uintptr_t vecData       = 0;
        std::uintptr_t childContainer = 0;
        std::uintptr_t actorList     = 0;
        std::size_t    rawEntries    = 0; // count returned in @p out
    };

    [[nodiscard]] ActorListDebugSummary debug_enumerate_actor_list(
        ActorListDebugEntry *out,
        std::size_t cap) noexcept;

    /**
     * @brief Override the per-character codename tokens used by the
     *        appearance-config classifier.
     * @details Each mod loads its own INI; pass the user-configured
     *          codenames here at config-load time (and again on
     *          auto-reload). Empty strings are ignored -- the
     *          corresponding codename keeps its current value.
     *          Built-in defaults:
     *            kliff   = "cd_phm_macduff"
     *            damiane = "cd_phw_damian"
     *            oongka  = "cd_phm_oongka"
     *          Provided in case a future game update or local mod
     *          renames a protagonist's appearance subfolder.
     */
    void set_protagonist_codenames(
        std::string_view kliff,
        std::string_view damiane,
        std::string_view oongka) noexcept;

    /**
     * @brief Drop the cached Kliff CCOIA pointer.
     * @details Forces the next chain walk to observe Kliff's CCOIA
     *          as a fresh pointer, which bumps world_generation()
     *          on a true save-load even if the engine happened to
     *          reuse the previous arena slot for the reallocation.
     *          Cheap; safe from any thread. Call on world-reload
     *          transitions if you want a defensively-fresh
     *          generation bump (the regular Kliff-CCOIA delta
     *          tracking already covers normal save-loads, so this
     *          is rarely required).
     */
    void invalidate_controlled_character() noexcept;

    /**
     * @brief Find a component instance inside a CCOIA component-pointer
     *        table by its MSVC RTTI mangled type-descriptor name.
     * @details Component slots in @p p1 are sparse and slot-drift across
     *          early/late load (a class can sit at +0x40 in steady state
     *          and +0x48 during cold load). Hardcoded slot offsets are
     *          fragile; this scanner identifies the target by traversing
     *          each non-null slot's vtable -> RTTICompleteObjectLocator
     *          -> TypeDescriptor and exact-comparing the mangled name.
     *
     *          The vtable address of the first match is cached via the
     *          caller-supplied @p vtableCache so subsequent calls fast-
     *          path on a single qword compare per slot. The cache lives
     *          for process lifetime (image-resident vtables); the caller
     *          declares one `std::atomic<std::uintptr_t>` static per
     *          component class.
     *
     *          SEH-guarded. Returns 0 on torn reads, missing class, or
     *          when @p p1 / @p rttiName is invalid.
     *
     * @param p1            Component-pointer table base
     *                      (CCOIA + 0x68 typically).
     * @param rttiName      MSVC-mangled type-descriptor name, e.g.
     *                      ".?AVClientCharacterControlActorComponent@pa@@".
     * @param vtableCache   Caller-owned cache slot. Must be initialised
     *                      to 0 and dedicated to one @p rttiName.
     * @returns Slot pointer (the component instance) or 0.
     */
    [[nodiscard]] std::uintptr_t find_component_in_table(
        std::uintptr_t p1,
        std::string_view rttiName,
        std::atomic<std::uintptr_t> &vtableCache) noexcept;

    /**
     * @brief Convenience: locate a component on the currently-controlled
     *        actor by its RTTI type-descriptor name.
     * @details Equivalent to:
     *
     *              ccoia = current_controlled_ccoia();
     *              p1    = *(ccoia + 0x68);
     *              return find_component_in_table(p1, rttiName, cache);
     *
     *          ...but keeps the component-table offset encapsulated
     *          inside CDCore so callers carry no hardcoded engine
     *          offsets. SEH-guarded end-to-end; returns 0 on a torn
     *          chain or missing component.
     *
     *          Intended for readiness probes: a component that
     *          resolves by RTTI is fully wired in the engine's
     *          component graph. Cold-load returns 0 for at least one
     *          component until the table finishes registering all
     *          classes.
     */
    [[nodiscard]] std::uintptr_t find_component_in_controlled_actor(
        std::string_view rttiName,
        std::atomic<std::uintptr_t> &vtableCache) noexcept;

    /**
     * @brief Locate a component on the actor whose
     *        ClientEquipSlotActorComponent is @p equipSlot.
     * @details Walks the equip-slot's CCOIA back-pointer and the
     *          CCOIA's component-pointer table internally, then
     *          RTTI-matches @p rttiName. Encapsulates the
     *          a1->CCOIA->p1 chain offsets inside CDCore so callers
     *          carry no engine offsets. SEH-guarded end-to-end.
     *          Used by readiness probes that need to inspect a
     *          specific sibling component of any protagonist (not
     *          just the controlled character).
     * @param equipSlot ClientEquipSlotActorComponent pointer (the
     *                  same a1 the SlotPopulator pipeline carries).
     * @param rttiName  MSVC-mangled type-descriptor name.
     * @param vtableCache Caller-owned cache slot per rttiName.
     */
    [[nodiscard]] std::uintptr_t find_component_for_equipslot(
        std::uintptr_t equipSlot,
        std::string_view rttiName,
        std::atomic<std::uintptr_t> &vtableCache) noexcept;

} // namespace CDCore

#endif // CDCORE_CONTROLLED_CHAR_HPP
