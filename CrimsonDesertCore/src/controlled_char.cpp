#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace CDCore
{
    namespace
    {
        // s_wsHolder holds the absolute address of the WorldSystem holder
        // static, published by a consumer's init code. While zero every
        // resolver query short-circuits to Unknown so the mod degrades
        // gracefully when the consumer has not yet completed its own AOB
        // scan (or that scan fails on a future patch).
        std::atomic<std::uintptr_t> s_wsHolder{0};

        // s_lastGoodChar caches the most recent known identity. The chain
        // walk can land mid-swap when the engine is rewriting user+0xD8 and
        // produce garbage; the cache absorbs those torn reads so the
        // resolver does not flap between Unknown and a known character
        // within a single swap. Stored as the underlying enum value to keep
        // the atomic lock-free on x64.
        std::atomic<std::uint8_t> s_lastGoodChar{
            static_cast<std::uint8_t>(ControlledCharacter::Unknown)};

        // Walk offsets starting at the WorldSystem holder static:
        //
        //   *(holder)            -> WorldSystem instance (session heap)
        //   *(ws   + 0x30)       -> ActorManager singleton
        //   *(am   + 0x28)       -> UserActor singleton (ClientUserActor)
        //   *(user + 0xD0)       -> primary actor slot (always Kliff in the
        //                           current story progression; this value
        //                           is the structural Kliff anchor)
        //   *(user + 0xD8)       -> controlled client actor (rotates on swap)
        //
        // The UserActor pointer is stable for the lifetime of a save: only
        // user+0xD8 rotates when the player switches which body they
        // control. When Kliff is the controlled character, user+0xD0 and
        // user+0xD8 reference the same actor; this coincidence is the
        // structural Kliff detector used below.
        constexpr std::ptrdiff_t k_wsToActorMgrOffset          = 0x30;
        constexpr std::ptrdiff_t k_actorMgrToUserOffset        = 0x28;
        constexpr std::ptrdiff_t k_userToPrimaryActorOffset    = 0xD0;
        constexpr std::ptrdiff_t k_userToControlledActorOffset = 0xD8;

        // Per-actor slot-index byte on ClientChildOnlyInGameActor.
        //
        // v1.04.00: byte at +0x50 holds the raw engine slot-index
        //
        //   Kliff   +0x50 = 1
        //   Damiane +0x50 = 2
        //   Oongka  +0x50 = 3
        //
        // Each body's +0x50 is stable across frames and distinct per
        // protagonist; +0x64 carries the same byte redundantly but
        // +0x50 is qword-aligned.
        //
        // v1.03.01 used +0x60 for this role. On v1.04.00 the +0x60 byte
        // reads 1 for every body and is no longer a discriminator.
        //
        // The primaryActor (UA+0xD0) is always Kliff on a Kliff-led
        // save, so `controlledSlot - primarySlot` yields the companion
        // diff:
        //
        //   Damiane - Kliff = 1
        //   Oongka  - Kliff = 2
        //
        // When Kliff is the controlled character UA+0xD0 == UA+0xD8
        // and decode_probe short-circuits via the pointer equality
        // check before the slot diff is evaluated.
        //
        // The 1.03.01 fallback caveat (`save with only Kliff + Oongka
        // decodes Oongka as Damiane`) has not been re-verified on a
        // 2-protagonist v1.04.00 save; keep the {1, 2} diff table
        // strict until that re-test lands.
        constexpr std::ptrdiff_t k_actorSlotIndexByteOffset = 0x50;
        constexpr int k_damianeSlotDiff = 1;
        constexpr int k_oongkaSlotDiff  = 2;

        // Lower bound for "looks like a heap pointer". Any value below the
        // 64 KiB Windows guard region is treated as null/invalid; this
        // catches both real null pointers and the small enum-shaped values
        // an uninitialised slot may carry before the singleton is wired up.
        constexpr std::uintptr_t k_minValidPtr = 0x10000;

        // One chain walk yields enough structural data to decode identity.
        // Populated fully only when the walk completes without faulting; on
        // any fault `valid` stays false and the numeric fields carry zero.
        struct ChainProbe
        {
            std::uintptr_t primaryActor;
            std::uintptr_t controlledActor;
            std::uint8_t   primarySlot;
            std::uint8_t   controlledSlot;
            bool           valid;
        };

        // SEH-isolated chain walk. Lives in its own function because MSVC
        // rejects the combination of __try and any C++ object with a
        // non-trivial destructor in the same scope (C2712). Each
        // intermediate pointer is rejected if it falls below the guard
        // region so a half-torn chain cannot reach the final byte reads
        // with garbage state.
        //
        // Reads BOTH user+0xD0 (primary actor, Kliff anchor) and
        // user+0xD8 (controlled actor) plus the slot-index byte on each
        // actor (see k_actorSlotIndexByteOffset). The two byte reads
        // cost negligible cache traffic because the actor was just
        // dereferenced and is already in cache.
        ChainProbe walk_chain_seh(std::uintptr_t holder) noexcept
        {
            ChainProbe out{};
            if (holder < k_minValidPtr)
            {
                return out;
            }
            __try
            {
                const auto ws =
                    *reinterpret_cast<const volatile std::uintptr_t *>(holder);
                if (ws < k_minValidPtr)
                {
                    return out;
                }
                const auto am =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        ws + k_wsToActorMgrOffset);
                if (am < k_minValidPtr)
                {
                    return out;
                }
                const auto user =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        am + k_actorMgrToUserOffset);
                if (user < k_minValidPtr)
                {
                    return out;
                }
                const auto primary =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        user + k_userToPrimaryActorOffset);
                if (primary < k_minValidPtr)
                {
                    return out;
                }
                const auto controlled =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        user + k_userToControlledActorOffset);
                if (controlled < k_minValidPtr)
                {
                    return out;
                }
                const auto primarySlot =
                    *reinterpret_cast<const volatile std::uint8_t *>(
                        primary + k_actorSlotIndexByteOffset);
                const auto controlledSlot =
                    *reinterpret_cast<const volatile std::uint8_t *>(
                        controlled + k_actorSlotIndexByteOffset);
                out.primaryActor    = primary;
                out.controlledActor = controlled;
                out.primarySlot     = primarySlot;
                out.controlledSlot  = controlledSlot;
                out.valid           = true;
                return out;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return out;
            }
        }

        // Decode identity from a completed chain probe using the
        // structural D0==D8 invariant for Kliff and the slot-index diff
        // for companions. Returns Unknown when the probe is not valid or
        // the slot diff falls outside the expected {1, 2} range.
        ControlledCharacter decode_probe(const ChainProbe &probe) noexcept
        {
            if (!probe.valid)
            {
                return ControlledCharacter::Unknown;
            }
            if (probe.controlledActor == probe.primaryActor)
            {
                return ControlledCharacter::Kliff;
            }
            const int diff = static_cast<int>(probe.controlledSlot) -
                             static_cast<int>(probe.primarySlot);
            if (diff == k_damianeSlotDiff)
            {
                return ControlledCharacter::Damiane;
            }
            if (diff == k_oongkaSlotDiff)
            {
                return ControlledCharacter::Oongka;
            }
            return ControlledCharacter::Unknown;
        }

        // Diagnostic: log each unique (controlledSlot - primarySlot) diff
        // observed from a valid probe, at most once per process. Four slots
        // covers the three expected values plus one transient anomaly.
        // Logs both the raw slot bytes and the computed diff so a future
        // patch that alters the offset layout is immediately visible.
        std::array<std::atomic<std::uint32_t>, 4> s_seenProbes{};

        constexpr std::uint32_t k_probeSentinel = 0xFFFFFFFFu;

        std::uint32_t pack_probe_signature(const ChainProbe &probe) noexcept
        {
            // primary in low byte, controlled in next byte, leaves the
            // high half zero. Guaranteed distinct from k_probeSentinel
            // because bit 31 is never set.
            return static_cast<std::uint32_t>(probe.primarySlot) |
                   (static_cast<std::uint32_t>(probe.controlledSlot) << 8);
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
                        const int diff =
                            static_cast<int>(probe.controlledSlot) -
                            static_cast<int>(probe.primarySlot);
                        // Prefer the short character name when the decode
                        // matched one of the three known identities.
                        // Unknown falls back to a literal "Unknown" label
                        // because controlled_character_name() returns an
                        // empty view for that case and we want a
                        // self-describing log line even for anomalies.
                        const auto name = controlled_character_name(decoded);
                        DMK::Logger::get_instance().info(
                            "ControlledChar: probe primary_slot={} "
                            "controlled_slot={} diff={} -> {}",
                            static_cast<unsigned>(probe.primarySlot),
                            static_cast<unsigned>(probe.controlledSlot),
                            diff,
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

    ControlledCharacter current_controlled_character() noexcept
    {
        const auto holder = s_wsHolder.load(std::memory_order_acquire);
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

        // A known-identity decode refreshes the last-known-good cache.
        // Anything else (invalid probe from a faulted chain walk, torn
        // read mid-swap that lands on an unexpected slot diff, or an
        // unrecognised future slot offset) is rejected and the
        // previously-cached identity is reused instead. When the cache
        // itself is empty (pre-world, post-invalidation) the final return
        // falls through to Unknown.
        if (decoded != ControlledCharacter::Unknown)
        {
            s_lastGoodChar.store(static_cast<std::uint8_t>(decoded),
                                 std::memory_order_release);
            return decoded;
        }

        return static_cast<ControlledCharacter>(
            s_lastGoodChar.load(std::memory_order_acquire));
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

    namespace
    {
        // Read just the primary actor's slot-index byte. Mirrors
        // walk_chain_seh's pointer-validity discipline (every intermediate
        // pointer below the 64 KiB guard region is rejected) so a torn
        // chain can never reach the final byte read with garbage state.
        // Output is the slot byte in `outPrimarySlot` plus a `valid` flag;
        // a faulted walk leaves the byte at zero and `valid` at false.
        struct PrimarySlotProbe
        {
            std::uint8_t slot;
            bool         valid;
        };

        PrimarySlotProbe read_primary_slot_seh(std::uintptr_t holder) noexcept
        {
            PrimarySlotProbe out{};
            if (holder < k_minValidPtr)
            {
                return out;
            }
            __try
            {
                const auto ws =
                    *reinterpret_cast<const volatile std::uintptr_t *>(holder);
                if (ws < k_minValidPtr)
                {
                    return out;
                }
                const auto am =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        ws + k_wsToActorMgrOffset);
                if (am < k_minValidPtr)
                {
                    return out;
                }
                const auto user =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        am + k_actorMgrToUserOffset);
                if (user < k_minValidPtr)
                {
                    return out;
                }
                const auto primary =
                    *reinterpret_cast<const volatile std::uintptr_t *>(
                        user + k_userToPrimaryActorOffset);
                if (primary < k_minValidPtr)
                {
                    return out;
                }
                out.slot =
                    *reinterpret_cast<const volatile std::uint8_t *>(
                        primary + k_actorSlotIndexByteOffset);
                out.valid = true;
                return out;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return out;
            }
        }

        // SEH-isolated single-byte read on an arbitrary body pointer.
        // Returns the slot byte plus a validity flag; a fault during the
        // read (recycled or freed body, partially-torn structure right
        // after a swap) leaves the byte at zero and the flag at false.
        struct BodySlotProbe
        {
            std::uint8_t slot;
            bool         valid;
        };

        BodySlotProbe read_body_slot_seh(std::uintptr_t body) noexcept
        {
            BodySlotProbe out{};
            if (body < k_minValidPtr)
            {
                return out;
            }
            __try
            {
                out.slot =
                    *reinterpret_cast<const volatile std::uint8_t *>(
                        body + k_actorSlotIndexByteOffset);
                out.valid = true;
                return out;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return out;
            }
        }

    } // anonymous namespace

    std::uint32_t resolve_character_idx_for_body(std::uintptr_t body) noexcept
    {
        if (body < k_minValidPtr)
        {
            return 0;
        }
        const auto holder = s_wsHolder.load(std::memory_order_acquire);
        if (holder == 0)
        {
            return 0;
        }
        // Two independent SEH probes: the primary-actor walk faulting
        // does not invalidate the body byte read and vice versa. Both
        // must succeed for the diff to mean anything.
        const auto primary = read_primary_slot_seh(holder);
        if (!primary.valid)
        {
            return 0;
        }
        const auto bodyProbe = read_body_slot_seh(body);
        if (!bodyProbe.valid)
        {
            return 0;
        }
        const int diff = static_cast<int>(bodyProbe.slot) -
                         static_cast<int>(primary.slot);
        if (diff == 0)
        {
            return 1; // Kliff
        }
        if (diff == k_damianeSlotDiff)
        {
            return 2; // Damiane
        }
        if (diff == k_oongkaSlotDiff)
        {
            return 3; // Oongka
        }
        return 0;
    }

    void invalidate_controlled_character() noexcept
    {
        // Release ordering pairs with the acquire load in
        // current_controlled_character() so any reader that subsequently
        // observes the Unknown sentinel also observes side effects the
        // caller performed before invalidating (e.g. clearing per-character
        // preset caches in preparation for a save-load transition).
        s_lastGoodChar.store(
            static_cast<std::uint8_t>(ControlledCharacter::Unknown),
            std::memory_order_release);
    }

} // namespace CDCore
