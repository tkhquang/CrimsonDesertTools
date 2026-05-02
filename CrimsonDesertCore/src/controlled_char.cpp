#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <safetyhook/context.hpp>
#include <safetyhook/mid_hook.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace CDCore
{
    namespace
    {
        // Server-side player-state holder static. Published by a consumer's
        // init code via set_player_static_holder(). While zero every
        // resolver query short-circuits to Unknown so the mod degrades
        // gracefully when the consumer has not yet completed its own AOB
        // scan (or that scan fails on a future patch).
        std::atomic<std::uintptr_t> s_playerStaticHolder{0};

        // WorldSystem holder retained for callers that walk the client-side
        // chain (UserActor body pool, mesh state). Identity decoding does
        // not consume this pointer; it is stored only because the public
        // set_world_system_holder() API still exists for source-level
        // compatibility with consumers that publish both addresses.
        std::atomic<std::uintptr_t> s_wsHolder{0};

        // Cache of the most recent known identity. The chain walk can land
        // mid-swap when the engine is rewriting active-flag bytes on the
        // three slots and produce a transient state with zero or two hot
        // slots; the cache absorbs those torn reads so the resolver does
        // not flap between Unknown and a known character within a single
        // swap. Stored as the underlying enum value to keep the atomic
        // lock-free on x64.
        std::atomic<std::uint8_t> s_lastGoodChar{
            static_cast<std::uint8_t>(ControlledCharacter::Unknown)};

        // Walk offsets from the player static down to the party container.
        // See controlled_char.hpp for the full chain narrative.
        //
        //   *(holder)            -> root container (no specific RTTI)
        //   *(root  + 0x18)      -> pa::NwVirtualAsyncSession
        //   *(nwSes + 0xA0)      -> pa::ServerUserActor
        //   *(srvUA + 0xD0)      -> pa::ServerChildOnlyInGameActor
        //                           (the party container)
        constexpr std::ptrdiff_t k_rootToNwSessionOffset    = 0x18;
        constexpr std::ptrdiff_t k_nwSessionToUserOffset    = 0xA0;
        constexpr std::ptrdiff_t k_userToPartyOffset        = 0xD0;

        // Per-protagonist slot offsets inside the party container. Stride
        // is 0x100 between consecutive slots; the absolute offsets are
        // baked into the engine layout and serve as the identity
        // discriminator. Adding a fourth protagonist would extend this
        // table at +0x368.
        constexpr std::ptrdiff_t k_kliffSlotOffset   = 0x68;
        constexpr std::ptrdiff_t k_damianeSlotOffset = 0x168;
        constexpr std::ptrdiff_t k_oongkaSlotOffset  = 0x268;

        // Per-slot fields:
        //   slot + 0x2C   u8   active flag (1 = controlled, 0 = passive)
        //   slot + 0x40   u32  observability handle (volatile; log-only)
        constexpr std::ptrdiff_t k_slotActiveFlagOffset       = 0x2C;
        constexpr std::ptrdiff_t k_slotObservabilityOffset    = 0x40;

        // Lower bound for "looks like a heap pointer". Any value below the
        // 64 KiB Windows guard region is treated as null/invalid; this
        // catches both real null pointers and the small enum-shaped values
        // an uninitialised slot may carry before the singleton is wired up.
        constexpr std::uintptr_t k_minValidPtr = 0x10000;

        // One chain walk yields the three slot bytes plus their
        // observability handles. Populated fully only when the walk
        // completes without faulting; on any fault `valid` stays false and
        // the numeric fields carry zero.
        struct ChainProbe
        {
            std::uintptr_t partyContainer;
            std::uint8_t   kliffActive;
            std::uint8_t   damianeActive;
            std::uint8_t   oongkaActive;
            std::uint32_t  kliffObservability;
            std::uint32_t  damianeObservability;
            std::uint32_t  oongkaObservability;
            bool           valid;
        };

        // SEH-isolated chain walk. Lives in its own function because MSVC
        // rejects the combination of __try and any C++ object with a
        // non-trivial destructor in the same scope (C2712). Each
        // intermediate pointer is rejected if it falls below the guard
        // region so a half-torn chain cannot reach the slot reads with
        // garbage state.
        ChainProbe walk_chain_seh(std::uintptr_t holder) noexcept
        {
            ChainProbe out{};
            if (holder < k_minValidPtr)
            {
                return out;
            }
            __try
            {
                const auto root =
                    *reinterpret_cast<const volatile std::uintptr_t *>(holder);
                if (root < k_minValidPtr)
                {
                    return out;
                }
                const auto nwSession =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        root + k_rootToNwSessionOffset);
                if (nwSession < k_minValidPtr)
                {
                    return out;
                }
                const auto userActor =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        nwSession + k_nwSessionToUserOffset);
                if (userActor < k_minValidPtr)
                {
                    return out;
                }
                const auto party =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        userActor + k_userToPartyOffset);
                if (party < k_minValidPtr)
                {
                    return out;
                }
                // Three slot reads, each one byte for the active flag and
                // one dword for the observability handle. The slot bytes
                // are nominally pinned at compile-time-stable offsets so
                // a fault here would indicate a torn party container that
                // the SEH guard correctly rejects.
                const auto kliffSlot   = party + k_kliffSlotOffset;
                const auto damianeSlot = party + k_damianeSlotOffset;
                const auto oongkaSlot  = party + k_oongkaSlotOffset;
                const auto kliffActive =
                    *reinterpret_cast<const volatile std::uint8_t *>(
                        kliffSlot + k_slotActiveFlagOffset);
                const auto damianeActive =
                    *reinterpret_cast<const volatile std::uint8_t *>(
                        damianeSlot + k_slotActiveFlagOffset);
                const auto oongkaActive =
                    *reinterpret_cast<const volatile std::uint8_t *>(
                        oongkaSlot + k_slotActiveFlagOffset);
                const auto kliffObs =
                    *reinterpret_cast<const volatile std::uint32_t *>(
                        kliffSlot + k_slotObservabilityOffset);
                const auto damianeObs =
                    *reinterpret_cast<const volatile std::uint32_t *>(
                        damianeSlot + k_slotObservabilityOffset);
                const auto oongkaObs =
                    *reinterpret_cast<const volatile std::uint32_t *>(
                        oongkaSlot + k_slotObservabilityOffset);
                out.partyContainer       = party;
                out.kliffActive          = kliffActive;
                out.damianeActive        = damianeActive;
                out.oongkaActive         = oongkaActive;
                out.kliffObservability   = kliffObs;
                out.damianeObservability = damianeObs;
                out.oongkaObservability  = oongkaObs;
                out.valid                = true;
                return out;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return out;
            }
        }

        // Decode identity from a completed chain probe. Returns Unknown
        // when the probe is invalid, when no slot is hot (cutscene /
        // loading screen), or when more than one slot is hot (a transient
        // engine state mid-swap that the cache layer is responsible for
        // smoothing over).
        ControlledCharacter decode_probe(const ChainProbe &probe) noexcept
        {
            if (!probe.valid)
            {
                return ControlledCharacter::Unknown;
            }
            const int hotCount = (probe.kliffActive   == 1 ? 1 : 0) +
                                 (probe.damianeActive == 1 ? 1 : 0) +
                                 (probe.oongkaActive  == 1 ? 1 : 0);
            if (hotCount != 1)
            {
                return ControlledCharacter::Unknown;
            }
            if (probe.kliffActive == 1)
            {
                return ControlledCharacter::Kliff;
            }
            if (probe.damianeActive == 1)
            {
                return ControlledCharacter::Damiane;
            }
            return ControlledCharacter::Oongka;
        }

        // Diagnostic: log each unique active-flag triplet observed from a
        // valid probe, at most once per process. Eight slots covers the
        // four expected states (none-hot, Kliff-hot, Damiane-hot,
        // Oongka-hot) with headroom for transient torn-read patterns. The
        // observability dwords are logged alongside the active flags so
        // session-to-session correlation is possible without re-running
        // the resolver in a debugger.
        std::array<std::atomic<std::uint32_t>, 8> s_seenProbes{};

        constexpr std::uint32_t k_probeSentinel = 0xFFFFFFFFu;

        // Pack the three active-flag bytes (each clamped to {0, 1}) into a
        // 24-bit signature. Guaranteed distinct from k_probeSentinel
        // because the high byte is always zero.
        std::uint32_t pack_probe_signature(const ChainProbe &probe) noexcept
        {
            const std::uint32_t k = (probe.kliffActive == 1) ? 1u : 0u;
            const std::uint32_t d = (probe.damianeActive == 1) ? 1u : 0u;
            const std::uint32_t o = (probe.oongkaActive == 1) ? 1u : 0u;
            return k | (d << 8) | (o << 16);
        }

        void log_first_seen_probe(const ChainProbe &probe,
                                  ControlledCharacter decoded) noexcept
        {
            const auto sig = pack_probe_signature(probe);
            for (auto &slot : s_seenProbes)
            {
                auto prev = slot.load(std::memory_order_relaxed);
                if (prev != 0 && (prev ^ k_probeSentinel) == sig)
                {
                    return;
                }
                if (prev == 0)
                {
                    const auto stamp = sig ^ k_probeSentinel;
                    if (slot.compare_exchange_strong(
                            prev, stamp,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed))
                    {
                        // Prefer the short character name when the decode
                        // matched one of the three known identities.
                        // Unknown falls back to a literal "Unknown" label
                        // because controlled_character_name() returns an
                        // empty view for that case and we want a
                        // self-describing log line even for anomalies.
                        const auto name = controlled_character_name(decoded);
                        DMK::Logger::get_instance().info(
                            "ControlledChar: probe kliff_active={} "
                            "damiane_active={} oongka_active={} "
                            "kliff_obs=0x{:X} damiane_obs=0x{:X} "
                            "oongka_obs=0x{:X} -> {}",
                            static_cast<unsigned>(probe.kliffActive),
                            static_cast<unsigned>(probe.damianeActive),
                            static_cast<unsigned>(probe.oongkaActive),
                            probe.kliffObservability,
                            probe.damianeObservability,
                            probe.oongkaObservability,
                            name.empty() ? std::string_view{"Unknown"}
                                         : name);
                        return;
                    }
                    if ((prev ^ k_probeSentinel) == sig)
                    {
                        return;
                    }
                }
            }
            // Seen-set exhausted. The resolver stays functional; only the
            // diagnostic log is silently dropped for additional unknown
            // probe signatures.
        }

        // -------------------------------------------------------------------
        // Radial-swap-key capture: actor pointer -> character cache.
        //
        // The mid-hook installed by install_radial_swap_hook() fires inside
        // the radial UI's swap handler at the moment EAX has just been
        // loaded with the requested characterinfo key (1 = Kliff,
        // 4 = Damiane, 6 = Oongka). At that point User+0xD8 still holds
        // the OLD actor pointer; the new actor commit happens later in
        // the same handler when sub_141B0C2B0 writes the resolved actor.
        //
        // Correlation strategy (chosen for cleaner concurrency vs. a
        // second hook on the call return):
        //
        //   1. Hook stamps a short-lived process-wide pending record
        //      with (key, deadline_tick). Single record because a radial
        //      swap is a synchronous user event with no nested handlers.
        //
        //   2. The next chain-walk in current_controlled_character() that
        //      observes a valid userActor and a non-Kliff slot+0x2C
        //      decode commits (actor_ptr -> key) into the actor cache,
        //      then clears the pending record. The deadline (currently
        //      2000 ms) bounds the window so a stale record from a
        //      cancelled radial cannot pollute future commits.
        //
        //   3. The resolver consults the cache before falling back to
        //      the slot-offset decode when the +0x2C decode itself
        //      yielded Unknown but the chain walk reached a valid
        //      userActor. Cache hits are deterministic per-actor;
        //      cache misses fall through to the slot decode.
        //
        // Concurrency model: shared_mutex on the cache because the hook
        // (game thread) and resolver (worker threads + main render) hit
        // the cache asymmetrically -- many readers, occasional writer.
        // The pending record itself is two atomics (key + deadline);
        // races on the pending-record path collapse to a benign no-op
        // because either the hook overwrites a stale pending value or
        // the resolver clears one we are about to commit. In both cases
        // the next radial swap rebuilds the record correctly.
        //
        // Why a per-actor cache instead of a single "latest key" atomic:
        // the LiveTransmog preset routing must distinguish between
        // saved-out and reloaded actor pointers (actor pool reuses old
        // addresses across save loads). A per-actor map naturally goes
        // stale at save-load -- pre-existing entries reference dead
        // pointers the engine will not reuse for the same character --
        // and invalidate_controlled_character() clears the cache to
        // bound stale-entry growth.
        // -------------------------------------------------------------------

        // 2-second window: typical radial swap commits within tens of
        // milliseconds, but post-swap the resolver only refreshes when
        // a consumer queries it; 2 s tolerates one full
        // load_detect_thread tick before declaring the pending record
        // stale. Bounded above by the next radial swap clobbering it.
        constexpr std::uint64_t k_pendingKeyWindowMs = 2000u;

        // Engine characterinfo keys, verified live (CE 2026-05-02 across
        // four protagonist transitions including a save B follow-up):
        //   key 1 -> Kliff
        //   key 4 -> Damiane
        //   key 6 -> Oongka
        // Other key values land outside the protagonist set and are
        // stored as Unknown so the cache never falsely attributes an
        // NPC body to one of the three.
        constexpr std::uint32_t k_keyKliff   = 1u;
        constexpr std::uint32_t k_keyDamiane = 4u;
        constexpr std::uint32_t k_keyOongka  = 6u;

        ControlledCharacter character_from_key(std::uint32_t key) noexcept
        {
            switch (key)
            {
                case k_keyKliff:   return ControlledCharacter::Kliff;
                case k_keyDamiane: return ControlledCharacter::Damiane;
                case k_keyOongka:  return ControlledCharacter::Oongka;
                default:           return ControlledCharacter::Unknown;
            }
        }

        // Pending-record fields. s_pendingKey is read in the resolver
        // hot path so it is a plain atomic instead of a struct under a
        // lock; the deadline atomic provides freshness gating without
        // taking a mutex on every query. A non-zero pendingKey with
        // deadline >= now is "live"; everything else is "expired".
        std::atomic<std::uint32_t> s_pendingKey{0};
        std::atomic<std::uint64_t> s_pendingDeadlineMs{0};

        // Per-actor identity cache populated by the resolver when a
        // pending record commits, and consulted by the resolver as a
        // Stage 3 layer when the chain walk reaches a valid userActor
        // but the slot decode returns Unknown (or for cross-validation
        // when both Damiane and Oongka are radial-controllable but
        // neither holds a hot +0x2C flag in the formal party). Ranged
        // small (the engine has at most three live protagonist actor
        // pointers per session); the unordered_map is sized for that.
        std::shared_mutex s_actorCacheMutex;
        std::unordered_map<std::uintptr_t, ControlledCharacter>
            s_actorKeyCache;

        // Learning cache: body pointer (user+0xD8 at the moment of an
        // identified-controlled snapshot) -> character. Populated as a
        // side effect of every successful current_controlled_character()
        // call that sees a non-Unknown identity AND can reach user+0xD8
        // via the WS chain. Each protagonist's body is added to the
        // cache the first time the user controls that character; from
        // then on resolve_character_idx_for_body() can attribute roster
        // bodies to their identities even while a different protagonist
        // is currently controlled.
        //
        // Lifetime: cleared together with s_actorKeyCache in
        // invalidate_controlled_character() because save-load
        // reallocates the body pool; pre-existing entries reference
        // dead pointers the engine may reuse for a different character.
        //
        // Why a learning cache instead of a structural decode: the
        // v1.04.00 client body has no reachable back-reference to its
        // protagonist slot in the party container, and the per-actor
        // +0x50 byte that earlier builds used as a slot index ceased
        // to discriminate on this build. Observing the controlled-body
        // / known-character pairing each resolver tick is the only
        // stable mapping available from the public game-state surface.
        std::shared_mutex s_bodyCacheMutex;
        std::unordered_map<std::uintptr_t, ControlledCharacter>
            s_bodyKeyCache;

        // SafetyHook owner. Held in a dedicated mutex because the
        // install/uninstall paths must serialise against each other,
        // and a second mutex separate from s_actorCacheMutex prevents
        // any caller of the public install/uninstall API from blocking
        // a resolver query already inside the cache critical section.
        //
        // Single-owner-per-DLL semantics: CrimsonDesertCore is linked
        // as a STATIC library, so this state is private to whichever
        // Logic DLL (LiveTransmog or EquipHide) owns this translation
        // unit's instance. When both consumers AOB-resolve the same
        // radial-swap site and call install_radial_swap_hook(addr),
        // each DLL builds its own MidHook independently. SafetyHook's
        // internal chaining at the JMP-target site lets the two
        // MidHooks coexist -- each consumer's callback fires on every
        // radial swap. The AOB cascade in cdcore/anchors.hpp is
        // ordered so the post-patch-tolerant patterns match first,
        // letting the second consumer resolve the same target after
        // the first has already patched the prelude bytes.
        std::mutex s_hookMutex;
        safetyhook::MidHook s_radialSwapHook;
        std::atomic<std::uintptr_t> s_radialSwapHookAddr{0};

        // SafetyHook MidHook destination. Lives at file scope (function
        // pointer cannot be a lambda capture target). Reads the low 32
        // bits of RAX -- the captured characterinfo key -- and stamps
        // the pending record with a deadline 2 s in the future. Must
        // not block, allocate, or call into anything that can re-enter
        // SafetyHook; the body is a single atomic store pair.
        void radial_swap_midhook(safetyhook::Context &ctx) noexcept
        {
            // EAX = ctx.rax low 32 bits. The mid-hook lands on
            // `mov [rbp+0x78], eax` so RAX holds the just-loaded key
            // and is not yet shadowed by the lookup-table call.
            const auto key =
                static_cast<std::uint32_t>(ctx.rax & 0xFFFFFFFFu);
            const auto now = GetTickCount64();
            // Order: stamp the deadline first so the resolver does not
            // observe a fresh key with a stale deadline. The acquire
            // load on the resolver side pairs with these releases.
            s_pendingDeadlineMs.store(now + k_pendingKeyWindowMs,
                                      std::memory_order_release);
            s_pendingKey.store(key, std::memory_order_release);
        }

        // Try to commit the pending key against the supplied actor
        // pointer. Called from the resolver hot path on every query
        // that sees a valid userActor. Cheap on the no-pending fast
        // path (one relaxed atomic load + branch). On commit, takes
        // the cache mutex exclusive briefly; commit failures (expired
        // record, unknown key) silently drop the pending record so a
        // late retry does not double-commit.
        void try_commit_pending(std::uintptr_t userActor) noexcept
        {
            if (userActor < k_minValidPtr)
            {
                return;
            }
            const auto key = s_pendingKey.load(std::memory_order_acquire);
            if (key == 0)
            {
                return;
            }
            const auto deadline =
                s_pendingDeadlineMs.load(std::memory_order_acquire);
            const auto now = GetTickCount64();
            if (now > deadline)
            {
                // Stale; clear so we do not consume it on a later
                // unrelated query.
                s_pendingKey.store(0, std::memory_order_release);
                s_pendingDeadlineMs.store(0, std::memory_order_release);
                return;
            }
            const auto ch = character_from_key(key);
            if (ch == ControlledCharacter::Unknown)
            {
                // Out-of-range key (engine repurposed the slot for an
                // NPC body or a future protagonist not in our table);
                // clear and do not pollute the cache.
                s_pendingKey.store(0, std::memory_order_release);
                s_pendingDeadlineMs.store(0, std::memory_order_release);
                return;
            }
            {
                std::unique_lock<std::shared_mutex> lk(s_actorCacheMutex);
                s_actorKeyCache[userActor] = ch;
            }
            s_pendingKey.store(0, std::memory_order_release);
            s_pendingDeadlineMs.store(0, std::memory_order_release);

            DMK::Logger::get_instance().info(
                "ControlledChar: radial-swap cache bind userActor=0x{:X} "
                "-> {} (key={})",
                static_cast<std::uint64_t>(userActor),
                controlled_character_name(ch),
                key);
        }

        ControlledCharacter lookup_actor_cache(
            std::uintptr_t userActor) noexcept
        {
            if (userActor < k_minValidPtr)
            {
                return ControlledCharacter::Unknown;
            }
            std::shared_lock<std::shared_mutex> lk(s_actorCacheMutex);
            const auto it = s_actorKeyCache.find(userActor);
            if (it == s_actorKeyCache.end())
            {
                return ControlledCharacter::Unknown;
            }
            return it->second;
        }

        // SEH-isolated reach to the userActor pointer alone. The full
        // walk_chain_seh() above stops at the party container; the
        // cache layer keys on the userActor (party container's parent
        // in the chain) because the actor pointer is what the engine
        // commits per radial swap and what the radial-swap-key hook's
        // pending record is meant to attribute to.
        std::uintptr_t walk_to_user_actor_seh(std::uintptr_t holder) noexcept
        {
            if (holder < k_minValidPtr)
            {
                return 0;
            }
            __try
            {
                const auto root = *reinterpret_cast<
                    const volatile std::uintptr_t *>(holder);
                if (root < k_minValidPtr)
                {
                    return 0;
                }
                const auto nwSession = *reinterpret_cast<
                    const volatile std::uintptr_t *>(
                    root + k_rootToNwSessionOffset);
                if (nwSession < k_minValidPtr)
                {
                    return 0;
                }
                const auto userActor = *reinterpret_cast<
                    const volatile std::uintptr_t *>(
                    nwSession + k_nwSessionToUserOffset);
                if (userActor < k_minValidPtr)
                {
                    return 0;
                }
                return userActor;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // SEH-isolated walk that resolves user+0xD8 -- the rotating
        // CLIENT body pointer used as the body-cache key. Walks the WS
        // chain (s_wsHolder -> ws -> ws+0x30 (ActorManager) -> am+0x28
        // (ClientUserActor) -> +0xD8). The PLAYER-STATIC chain reaches
        // a distinct server-side ServerUserActor whose +0xD8 is not
        // the client body and would mis-key the cache (EquipHide's
        // body_to_vis_ctrl walker and char-swap detector both consume
        // the client body, so the cache must be keyed on it).
        //
        // Returns 0 when the WS holder has not been published yet,
        // when any intermediate pointer is below the guard region, or
        // when the load faults. Callers treat 0 as "no stamp this
        // tick" and leave the cache unchanged.
        std::uintptr_t walk_to_controlled_body_seh() noexcept
        {
            const auto wsHolder =
                s_wsHolder.load(std::memory_order_acquire);
            if (wsHolder < k_minValidPtr)
            {
                return 0;
            }
            __try
            {
                const auto ws = *reinterpret_cast<
                    const volatile std::uintptr_t *>(wsHolder);
                if (ws < k_minValidPtr)
                {
                    return 0;
                }
                // ws + 0x30 -> ActorManager. Same offset as walks
                // elsewhere in the codebase (EH player_detection, LT
                // transmog_worker).
                const auto am = *reinterpret_cast<
                    const volatile std::uintptr_t *>(ws + 0x30);
                if (am < k_minValidPtr)
                {
                    return 0;
                }
                // am + 0x28 -> ClientUserActor.
                const auto userActor = *reinterpret_cast<
                    const volatile std::uintptr_t *>(am + 0x28);
                if (userActor < k_minValidPtr)
                {
                    return 0;
                }
                // user + 0xD8 -> controlled CLIENT body. The userActor
                // singleton stays constant across radial swaps; only
                // this field rotates per swap, so it is the correct
                // key for body-identity attribution.
                const auto controlledBody = *reinterpret_cast<
                    const volatile std::uintptr_t *>(userActor + 0xD8);
                if (controlledBody < k_minValidPtr)
                {
                    return 0;
                }
                return controlledBody;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // Stamp (body -> character) into the learning cache. Called from
        // current_controlled_character() at every success exit. The
        // shared_mutex pattern matches s_actorKeyCache: many readers
        // (resolve_character_idx_for_body) and an occasional writer
        // (this stamp). Skips when @p body is below the guard region or
        // when @p ch is Unknown -- callers should only stamp on a
        // confirmed identity.
        void stamp_body_cache(std::uintptr_t body,
                              ControlledCharacter ch) noexcept
        {
            if (body < k_minValidPtr ||
                ch == ControlledCharacter::Unknown)
            {
                return;
            }
            std::unique_lock<std::shared_mutex> lk(s_bodyCacheMutex);
            s_bodyKeyCache[body] = ch;
        }

    } // anonymous namespace

    void set_world_system_holder(std::uintptr_t holderAddr) noexcept
    {
        s_wsHolder.store(holderAddr, std::memory_order_release);

        if (holderAddr != 0)
        {
            DMK::Logger::get_instance().info(
                "ControlledChar: WorldSystem holder published at 0x{:X}",
                static_cast<std::uint64_t>(holderAddr));
        }
    }

    void set_player_static_holder(std::uintptr_t holderAddr) noexcept
    {
        s_playerStaticHolder.store(holderAddr, std::memory_order_release);

        if (holderAddr != 0)
        {
            DMK::Logger::get_instance().info(
                "ControlledChar: Player static holder published at 0x{:X}",
                static_cast<std::uint64_t>(holderAddr));
        }
    }

    ControlledCharacter current_controlled_character() noexcept
    {
        const auto holder =
            s_playerStaticHolder.load(std::memory_order_acquire);
        if (holder == 0)
        {
            return ControlledCharacter::Unknown;
        }

        const auto probe   = walk_chain_seh(holder);
        const auto decoded = decode_probe(probe);

        if (probe.valid)
        {
            log_first_seen_probe(probe, decoded);
        }

        // Resolver layering, top-to-bottom:
        //
        //   1. Slot-offset decode (above): returns the character whose
        //      +0x2C flag is uniquely hot. Wins outright for all in-
        //      world states where the engine maintains the formal
        //      party invariant.
        //
        //   2. Pending-key commit + actor cache lookup (this block):
        //      walks the chain to the userActor and consults the
        //      radial-swap cache built by the mid-hook. Resolves the
        //      ghost-roster case where the formal party shows zero or
        //      multiple hot slots but the radial UI deterministically
        //      picked a specific character moments earlier.
        //
        //   3. Last-known-good cache (below): preserves the previously
        //      decoded identity when both prior layers return Unknown.
        //      Bridges transient torn reads mid-swap and any decode
        //      path that has never seen a known identity yet.
        const auto userActor = walk_to_user_actor_seh(holder);
        try_commit_pending(userActor);

        // A known slot-offset decode wins. Refresh the LKG cache so a
        // subsequent torn-read query can fall back to it.
        if (decoded != ControlledCharacter::Unknown)
        {
            s_lastGoodChar.store(static_cast<std::uint8_t>(decoded),
                                 std::memory_order_release);
            // Body-cache stamp: the chain walk just confirmed `decoded`
            // is the controlled identity, so user+0xD8 right now points
            // to that character's body. Reading it here lets
            // resolve_character_idx_for_body() attribute the body for
            // EquipHide's UA stride walk on later ticks even when a
            // different protagonist is being controlled.
            const auto controlledBody = walk_to_controlled_body_seh();
            stamp_body_cache(controlledBody, decoded);
            return decoded;
        }

        // Slot decode unknown -- consult the actor cache. A hit here is
        // the deterministic answer for ghost-roster scenes where the
        // formal +0x2C invariant does not apply but the radial UI's
        // captured key is still authoritative for the live actor. Also
        // refreshes the LKG cache on hit so a later query that the
        // userActor walk happens to fault on still resolves correctly.
        const auto cached = lookup_actor_cache(userActor);
        if (cached != ControlledCharacter::Unknown)
        {
            s_lastGoodChar.store(static_cast<std::uint8_t>(cached),
                                 std::memory_order_release);
            // Body-cache stamp: the actor cache resolved an identity
            // for the live userActor, so user+0xD8 currently holds the
            // body for `cached`. Same rationale as the slot-decode
            // branch above -- learn the body here so later body-keyed
            // queries resolve without re-running this chain walk.
            const auto controlledBody = walk_to_controlled_body_seh();
            stamp_body_cache(controlledBody, cached);
            return cached;
        }

        // Neither the slot decode nor the actor cache yielded a known
        // identity; fall back to the last-known-good. Empty pre-world
        // and post-invalidation, in which case the final return is
        // Unknown.
        return static_cast<ControlledCharacter>(
            s_lastGoodChar.load(std::memory_order_acquire));
    }

    bool install_radial_swap_hook(std::uintptr_t hookAddr) noexcept
    {
        std::lock_guard<std::mutex> lk(s_hookMutex);

        const auto current =
            s_radialSwapHookAddr.load(std::memory_order_acquire);

        if (hookAddr == 0)
        {
            // Teardown path. Idempotent: when nothing is installed the
            // moved-from MidHook destructor below is a no-op.
            if (current == 0)
            {
                return true;
            }
            // Destroy the MidHook so the patched bytes return to their
            // original instruction sequence. safetyhook::MidHook's
            // destructor handles unhook plus trampoline-page free under
            // SafetyHook's own internal synchronization. When two
            // consumer DLLs both have a MidHook here, each tears down
            // its own copy independently -- SafetyHook's chaining
            // unwinds the JMP target back through the remaining hooks.
            s_radialSwapHook = safetyhook::MidHook{};
            s_radialSwapHookAddr.store(0, std::memory_order_release);

            DMK::Logger::get_instance().info(
                "ControlledChar: radial-swap hook uninstalled");
            return true;
        }

        if (hookAddr < k_minValidPtr)
        {
            DMK::Logger::get_instance().warning(
                "ControlledChar: radial-swap hook rejected -- address "
                "0x{:X} below guard region",
                static_cast<std::uint64_t>(hookAddr));
            return false;
        }

        if (current == hookAddr)
        {
            // Already installed at this exact address. Idempotent.
            return true;
        }

        if (current != 0)
        {
            // Address shifted (AOB re-resolution mid-session). Tear
            // down the existing MidHook before rebuilding so we never
            // hold two MidHooks owned by the same DLL slot.
            DMK::Logger::get_instance().info(
                "ControlledChar: radial-swap hook target shifted "
                "0x{:X} -> 0x{:X}, rebuilding",
                static_cast<std::uint64_t>(current),
                static_cast<std::uint64_t>(hookAddr));
            s_radialSwapHook = safetyhook::MidHook{};
            s_radialSwapHookAddr.store(0, std::memory_order_release);
        }

        // Build the SafetyHook MidHook. SafetyHook's internal chaining
        // tolerates a sibling DLL having already patched this site
        // with its own MidHook; the JMP at the hook target is rewritten
        // to chain through both callbacks transparently.
        auto built = safetyhook::MidHook::create(
            reinterpret_cast<void *>(hookAddr),
            radial_swap_midhook);
        if (!built)
        {
            DMK::Logger::get_instance().warning(
                "ControlledChar: radial-swap hook creation failed at "
                "0x{:X}",
                static_cast<std::uint64_t>(hookAddr));
            return false;
        }

        s_radialSwapHook = std::move(*built);
        s_radialSwapHookAddr.store(hookAddr, std::memory_order_release);

        DMK::Logger::get_instance().info(
            "ControlledChar: radial-swap hook installed at 0x{:X}",
            static_cast<std::uint64_t>(hookAddr));
        return true;
    }

    void uninstall_radial_swap_hook() noexcept
    {
        (void)install_radial_swap_hook(0);
    }

    std::string_view controlled_character_name(
        ControlledCharacter ch) noexcept
    {
        switch (ch)
        {
            case ControlledCharacter::Kliff:   return "Kliff";
            case ControlledCharacter::Damiane: return "Damiane";
            case ControlledCharacter::Oongka: return "Oongka";
            case ControlledCharacter::Unknown: return {};
        }
        return {};
    }

    std::string_view current_controlled_character_name() noexcept
    {
        return controlled_character_name(current_controlled_character());
    }

    std::uint32_t current_controlled_character_idx() noexcept
    {
        switch (current_controlled_character())
        {
            case ControlledCharacter::Kliff:   return 1;
            case ControlledCharacter::Damiane: return 2;
            case ControlledCharacter::Oongka: return 3;
            case ControlledCharacter::Unknown: return 0;
        }
        return 0;
    }

    std::uint32_t resolve_character_idx_for_body(
        std::uintptr_t body) noexcept
    {
        // Learning-cache lookup. The cache is populated as a side
        // effect of every successful current_controlled_character()
        // call -- the resolver stamps (user+0xD8 -> identity) once it
        // confirms the controlled character, and after the user has
        // cycled through each protagonist at least once every
        // protagonist's body pointer is cache-resident. Roster bodies
        // (party members visible in the world but not currently
        // controlled) resolve here without requiring a structural
        // decode the v1.04.00 layout does not expose.
        //
        // Zero return on miss is the safe degradation: callers in
        // EquipHide treat zero as "unknown body" and fall back to the
        // active character's hide mask, which matches single-character
        // semantics until the cache warms.
        if (body < k_minValidPtr)
        {
            return 0;
        }
        std::shared_lock<std::shared_mutex> lk(s_bodyCacheMutex);
        const auto it = s_bodyKeyCache.find(body);
        if (it == s_bodyKeyCache.end())
        {
            return 0;
        }
        switch (it->second)
        {
            case ControlledCharacter::Kliff:   return 1;
            case ControlledCharacter::Damiane: return 2;
            case ControlledCharacter::Oongka:  return 3;
            case ControlledCharacter::Unknown: return 0;
        }
        return 0;
    }

    std::size_t snapshot_body_cache(BodyCacheEntry *out, std::size_t cap) noexcept
    {
        if (out == nullptr || cap == 0)
        {
            return 0;
        }
        std::shared_lock<std::shared_mutex> lk(s_bodyCacheMutex);
        std::size_t written = 0;
        for (const auto &kv : s_bodyKeyCache)
        {
            if (written >= cap)
            {
                break;
            }
            std::uint32_t idx = 0;
            switch (kv.second)
            {
                case ControlledCharacter::Kliff:   idx = 1; break;
                case ControlledCharacter::Damiane: idx = 2; break;
                case ControlledCharacter::Oongka:  idx = 3; break;
                case ControlledCharacter::Unknown: continue;
            }
            out[written++] = BodyCacheEntry{kv.first, idx};
        }
        return written;
    }

    void invalidate_swap_caches() noexcept
    {
        // Release ordering pairs with the acquire load in
        // current_controlled_character() so any reader that subsequently
        // observes the Unknown sentinel also observes side effects the
        // caller performed before invalidating (e.g. clearing per-character
        // preset caches in preparation for the swap settling).
        s_lastGoodChar.store(
            static_cast<std::uint8_t>(ControlledCharacter::Unknown),
            std::memory_order_release);

        // In-session radial swaps rotate user+0xD8 between existing body
        // pointers in the pool but reallocate the userActor parent only
        // on save-load. The actor->character cache keys on the userActor
        // pointer; clearing it on every swap is the safe choice because
        // the cache is bounded by the number of distinct userActor
        // singletons the engine produces (typically one per session) and
        // a stale entry would otherwise short-circuit the resolver to a
        // pre-swap identity. The pending radial-swap-key record is also
        // cleared because any in-flight commit it referenced is moot
        // once the swap completes.
        {
            std::unique_lock<std::shared_mutex> lk(s_actorCacheMutex);
            s_actorKeyCache.clear();
        }

        s_pendingKey.store(0, std::memory_order_release);
        s_pendingDeadlineMs.store(0, std::memory_order_release);
    }

    void invalidate_controlled_character() noexcept
    {
        // Full world-reload clear. Save-load transitions reallocate
        // every chain pointer including the body pool; flushing the
        // body cache here prevents post-load mis-attribution where
        // the engine reuses a previously-cached body address for a
        // different protagonist. The swap-scope state (LKG, actor
        // cache, pending record) is cleared by the helper below.
        invalidate_swap_caches();

        {
            std::unique_lock<std::shared_mutex> lk(s_bodyCacheMutex);
            s_bodyKeyCache.clear();
        }
    }

} // namespace CDCore
