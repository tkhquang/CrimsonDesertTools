#include <cdcore/anchors.hpp>
#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>
#include <DetourModKit/scanner.hpp>

#include <safetyhook/context.hpp>
#include <safetyhook/mid_hook.hpp>

#include <Windows.h>
#include <Psapi.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Controlled-character resolver (v1.06.01 verified).
//
// Identity is decoded from the engine's own focus-actor broadcast event
// (`sub_14353BA60` -- the per-subscriber focus-actor list mutator). R9
// at function entry carries the new focus-actor's u32 hash handle; the
// MidHook compares against the three AOB-resolved player hash globals
// (Kliff/Oongka/Damiane) and stamps a process-wide cache.
//
// Resolver layers (top wins) used by `current_controlled_character()`:
//
//   1. Structural Kliff anchor -- `body+0x80 == 0x30` reached through
//      the WorldSystem chain. Positive Kliff signal that does not need
//      a broadcast (covers cold-load to Kliff).
//   2. Tier-0 focus-broadcast cache, but only if non-Kliff. Skips a
//      stale Kliff intermediate fired by the engine during a same-
//      non-Kliff save reload.
//   3. `s_lastKnownNonKliff` fallback. Preserved across
//      `invalidate_controlled_character()` so a same-non-Kliff save
//      reload (engine never re-broadcasts the actual non-Kliff)
//      still recovers the correct identity.
//   4. Tier-0 even if Kliff (engine's authoritative signal).
//   5. LKG cache (final fallback for torn reads / pre-broadcast).
//
// Body learning cache. Each successful resolve stamps
// (user+0xD8 -> character) into a learning map consumed by
// `resolve_character_idx_for_body()` and `snapshot_body_cache()`. The
// cache reaches `user+0xD8` via the WorldSystem chain published by
// `set_world_system_holder()`.
//
// Radial-swap timestamp. The radial-swap MidHook callback is a single
// atomic store of `GetTickCount64()`; `radial_swap_pending()` reports
// true for the next 2 s. Consumers (EquipHide, LiveTransmog) use that
// flag to disambiguate a user-initiated swap from a save-load arena
// rotation when both surface as a `user+0xD8` pointer change.
//
// Thread safety:
//   - All setters (`set_world_system_holder`, `set_focus_actor_hash_
//     globals`) are safe from any thread; later writers win.
//   - `current_controlled_character()` is non-blocking and SEH-
//     guarded; safe from any thread including the rendering thread.
// ---------------------------------------------------------------------------

namespace CDCore
{
    namespace
    {
        // WorldSystem holder. Published by set_world_system_holder(). The
        // chain walk in walk_to_controlled_body_seh() reaches the rotating
        // user+0xD8 client body pointer via this chain; the body cache
        // keys on those body pointers.
        std::atomic<std::uintptr_t> s_wsHolder{0};

        // Tier-0 focus-broadcast cache. Stamped by the focus-broadcast
        // MidHook every time the engine fires a focus event whose R9
        // matches one of the three published character hash globals.
        // Stored as the underlying enum value to keep the atomic lock-
        // free on x64.
        std::atomic<std::uint8_t> s_focusBroadcastChar{
            static_cast<std::uint8_t>(ControlledCharacter::Unknown)};

        // Last-known-good identity. Used as the final fallback when a
        // chain walk transiently faults or the focus-broadcast cache
        // has not been stamped yet.
        std::atomic<std::uint8_t> s_lastGoodChar{
            static_cast<std::uint8_t>(ControlledCharacter::Unknown)};

        // Last broadcasted NON-Kliff identity (Damiane or Oongka).
        // Updated only when the focus-broadcast hook captures a non-
        // Kliff hash; Kliff captures do NOT touch this. Preserved
        // across invalidate_controlled_character() so the resolver
        // can recover the user's last Damiane/Oongka selection when
        // the engine wires Kliff intermediate during a same-non-
        // Kliff save reload (engine fires Kliff broadcast for the
        // intermediate, then transitions to the actual non-Kliff
        // body without firing a follow-up broadcast -- the
        // structural body marker tells us "not Kliff" but cannot
        // discriminate Damiane vs Oongka, so we fall back to this
        // cache).
        std::atomic<std::uint8_t> s_lastKnownNonKliff{
            static_cast<std::uint8_t>(ControlledCharacter::Unknown)};

        // Lower bound for "looks like a heap pointer". Any value below
        // the 64 KiB Windows guard region is treated as null/invalid;
        // this catches both real null pointers and small enum-shaped
        // values an uninitialised slot may carry before the engine
        // singleton is wired up.
        constexpr std::uintptr_t k_minValidPtr = 0x10000;

        // -------------------------------------------------------------------
        // Focus-broadcast (Tier-0) state.
        //
        // Three engine globals hold per-protagonist u32 hash handles
        // assigned at static init by the bridge functions described in
        // cdcore/anchors.hpp::k_focusActorInitPattern. Consumers AOB the
        // bridges (one-shot, on init) and publish the resolved global
        // addresses here via set_focus_actor_hash_globals(). The values
        // themselves are read on every focus-broadcast hook tick (cheap
        // relaxed loads) and compared against ctx.r9.
        // -------------------------------------------------------------------
        std::atomic<std::uintptr_t> s_kliffHashGlobalAddr{0};
        std::atomic<std::uintptr_t> s_oongkaHashGlobalAddr{0};
        std::atomic<std::uintptr_t> s_damianHashGlobalAddr{0};

        std::mutex s_focusHookMutex;
        safetyhook::MidHook s_focusBroadcastHook;
        std::atomic<std::uintptr_t> s_focusBroadcastHookAddr{0};

        // -------------------------------------------------------------------
        // Radial-swap timestamp (powers `radial_swap_pending()`).
        //
        // EquipHide consumes this signal to disambiguate two events that
        // both rotate user+0xD8: (a) the user pressing radial-swap keys
        // and (b) save-load reattach. When the controlled-actor pointer
        // shifts and the new pointer is absent from the body cache,
        // EquipHide checks `radial_swap_pending()`; true -> "first-time
        // radial swap to this protagonist" (preserve cache), false ->
        // "save-load arena rotation" (full reload wipe).
        //
        // The mid-hook callback is a single atomic store -- no character
        // decode, no chain walk. The character identity itself comes
        // from the focus-broadcast hook (Tier 0) which fires for the
        // same swap event.
        // -------------------------------------------------------------------
        constexpr std::uint64_t k_radialSwapPendingWindowMs = 2000u;
        std::atomic<std::uint64_t> s_radialSwapTimestampMs{0};

        std::mutex s_hookMutex;
        safetyhook::MidHook s_radialSwapHook;
        std::atomic<std::uintptr_t> s_radialSwapHookAddr{0};

        // -------------------------------------------------------------------
        // Body learning cache (body_ptr -> character).
        //
        // Populated as a side effect of every successful current_
        // controlled_character() call: when the resolver confirms an
        // identity it stamps the user+0xD8 body pointer into the cache.
        // Consumed by resolve_character_idx_for_body() so EquipHide's
        // roster walker can attribute non-controlled bodies (party
        // members visible in the world).
        //
        // Cleared on full world-reload via invalidate_controlled_
        // character() because save-load reallocates the body pool;
        // pre-existing entries reference dead pointers the engine may
        // reuse for a different protagonist.
        // -------------------------------------------------------------------
        std::shared_mutex s_bodyCacheMutex;
        std::unordered_map<std::uintptr_t, ControlledCharacter>
            s_bodyKeyCache;

        // SEH-isolated read of one of the three published hash globals.
        // Returns 0 when the address is unpublished or the load faults.
        std::uint32_t safe_read_hash_global(std::uintptr_t addr) noexcept
        {
            if (addr < k_minValidPtr)
                return 0;
            __try
            {
                return *reinterpret_cast<const volatile std::uint32_t *>(addr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // SEH-isolated read of an asciiz at @p addr into @p outBuf.
        // Caller-owned storage avoids RAII inside the SEH scope (MSVC
        // C2712). Writes the byte count (excluding NUL) into @p outLen.
        bool seh_read_cstring(std::uintptr_t addr,
                              char         *outBuf,
                              std::size_t   capacity,
                              std::size_t  &outLen) noexcept
        {
            outLen = 0;
            if (addr < k_minValidPtr || outBuf == nullptr || capacity == 0)
                return false;
            __try
            {
                const auto *cstr =
                    reinterpret_cast<const char *>(addr);
                std::size_t i = 0;
                while (i < capacity - 1 && cstr[i] != '\0')
                {
                    outBuf[i] = cstr[i];
                    ++i;
                }
                outBuf[i] = '\0';
                outLen = i;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                outBuf[0] = '\0';
                outLen = 0;
                return false;
            }
        }

        // SEH-isolated walk that resolves user+0xD8 -- the rotating
        // CLIENT body pointer used as the body-cache key. Walks the WS
        // chain (s_wsHolder -> ws -> ws+0x30 (ActorManager) -> am+0x28
        // (ClientUserActor) -> +0xD8). The PLAYER-STATIC chain reaches
        // a distinct server-side ServerUserActor whose +0xD8 is not the
        // client body and would mis-key the cache.
        std::uintptr_t walk_to_controlled_body_seh() noexcept
        {
            const auto wsHolder =
                s_wsHolder.load(std::memory_order_acquire);
            if (wsHolder < k_minValidPtr)
                return 0;
            __try
            {
                const auto ws = *reinterpret_cast<
                    const volatile std::uintptr_t *>(wsHolder);
                if (ws < k_minValidPtr)
                    return 0;
                const auto am = *reinterpret_cast<
                    const volatile std::uintptr_t *>(ws + 0x30);
                if (am < k_minValidPtr)
                    return 0;
                const auto userActor = *reinterpret_cast<
                    const volatile std::uintptr_t *>(am + 0x28);
                if (userActor < k_minValidPtr)
                    return 0;
                const auto controlledBody = *reinterpret_cast<
                    const volatile std::uintptr_t *>(userActor + 0xD8);
                if (controlledBody < k_minValidPtr)
                    return 0;
                return controlledBody;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // SEH-isolated structural read: is the given body Kliff?
        // Body+0x80 dword is 0x30 for Kliff bodies and 0x1D (or
        // similar non-0x30 sentinel) for Damiane/Oongka bodies, per
        // live captures 2026-05-14. This is a binary "is-Kliff"
        // signal: returns true ONLY when the marker explicitly says
        // Kliff. Returns false for non-Kliff AND for any fault /
        // unmapped read (so the caller falls back to Tier-0 / last-
        // known-non-Kliff cache, never up-casting an unknown to
        // Kliff). Cannot discriminate Damiane from Oongka -- both
        // share the non-Kliff marker.
        constexpr std::uint32_t k_bodyKliffMarker = 0x30;
        constexpr std::ptrdiff_t k_bodyMarkerOffset = 0x80;

        bool body_is_structurally_kliff(std::uintptr_t body) noexcept
        {
            if (body < k_minValidPtr)
                return false;
            __try
            {
                const auto marker =
                    *reinterpret_cast<const volatile std::uint32_t *>(
                        body + k_bodyMarkerOffset);
                return marker == k_bodyKliffMarker;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Stamp (body -> character) into the learning cache. Skips when
        // @p body is below the guard region or @p ch is Unknown.
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

        // SafetyHook MidHook destination for sub_14353BA60 (the engine's
        // per-subscriber focus-actor list mutator). ctx.r9 carries the
        // new focus-actor hash handle; we compare against the three
        // published player-character globals and, on match, stamp the
        // resolved character into s_focusBroadcastChar.
        //
        // The vast majority of fires (~99.8% on captured sessions) carry
        // NPC focus handles and are filtered out by the equality checks.
        // Must not block, allocate, or call into anything that can
        // re-enter SafetyHook.
        void focus_broadcast_midhook(safetyhook::Context &ctx) noexcept
        {
            const auto incoming = static_cast<std::uint32_t>(ctx.r9);
            if (incoming == 0)
                return;

            const auto kliff =
                safe_read_hash_global(s_kliffHashGlobalAddr.load(
                    std::memory_order_acquire));
            const auto oongka =
                safe_read_hash_global(s_oongkaHashGlobalAddr.load(
                    std::memory_order_acquire));
            const auto damian =
                safe_read_hash_global(s_damianHashGlobalAddr.load(
                    std::memory_order_acquire));

            ControlledCharacter resolved = ControlledCharacter::Unknown;
            if (kliff != 0 && incoming == kliff)
                resolved = ControlledCharacter::Kliff;
            else if (oongka != 0 && incoming == oongka)
                resolved = ControlledCharacter::Oongka;
            else if (damian != 0 && incoming == damian)
                resolved = ControlledCharacter::Damiane;
            else
                return; // Not a player-character broadcast.

            const auto prior = s_focusBroadcastChar.exchange(
                static_cast<std::uint8_t>(resolved),
                std::memory_order_acq_rel);
            s_lastGoodChar.store(
                static_cast<std::uint8_t>(resolved),
                std::memory_order_release);

            // Update s_lastKnownNonKliff ONLY for Damiane/Oongka
            // captures. This cache is the resolver's fallback when
            // the structural body marker says "not Kliff" but the
            // engine has not fired a Damiane/Oongka broadcast for
            // the actual current character (the same-non-Kliff
            // save reload edge case). Kliff captures must NOT
            // overwrite this cache, otherwise an intermediate Kliff
            // broadcast during world load would erase the user's
            // last Damiane/Oongka selection.
            if (resolved == ControlledCharacter::Damiane ||
                resolved == ControlledCharacter::Oongka)
            {
                s_lastKnownNonKliff.store(
                    static_cast<std::uint8_t>(resolved),
                    std::memory_order_release);
            }

            // Always emit at INFO on a change, TRACE otherwise. The
            // broadcast is event-rate (rare), not tick-rate, so log
            // volume is irrelevant; dedupe was suppressing the first
            // post-hot-reload capture when CDCore static state
            // retained the pre-reload value, making it look like the
            // hook had not fired.
            const bool changed =
                (prior != static_cast<std::uint8_t>(resolved));
            auto &logger = DMK::Logger::get_instance();
            if (changed)
            {
                logger.info(
                    "ControlledChar: focus-broadcast captured "
                    "hash=0x{:X} -> {} (changed={})",
                    incoming,
                    controlled_character_name(resolved),
                    changed);
            }
            else
            {
                logger.trace(
                    "ControlledChar: focus-broadcast captured "
                    "hash=0x{:X} -> {} (changed={})",
                    incoming,
                    controlled_character_name(resolved),
                    changed);
            }
        }

        // SafetyHook MidHook destination for the radial-swap handler
        // (sub_141B04040). Stamps a single monotonic timestamp consumed
        // by `radial_swap_pending()`. Consumers use that flag to
        // distinguish a user radial swap from a save-load arena
        // rotation; the character identity itself comes from the
        // focus-broadcast hook.
        void radial_swap_midhook(safetyhook::Context & /*ctx*/) noexcept
        {
            const auto now = GetTickCount64();
            s_radialSwapTimestampMs.store(now, std::memory_order_release);

            // User-input rate (sub-Hz) -- trace log overhead is
            // irrelevant. Surfacing every fire lets consumers tell a
            // radial swap (fires) from a save-load reattach (does not).
            DMK::Logger::get_instance().trace(
                "ControlledChar: radial-swap input fired (ts={})", now);
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

    void set_focus_actor_hash_globals(std::uintptr_t kliffAddr,
                                      std::uintptr_t oongkaAddr,
                                      std::uintptr_t damianAddr) noexcept
    {
        s_kliffHashGlobalAddr.store(kliffAddr,  std::memory_order_release);
        s_oongkaHashGlobalAddr.store(oongkaAddr, std::memory_order_release);
        s_damianHashGlobalAddr.store(damianAddr, std::memory_order_release);

        if (kliffAddr || oongkaAddr || damianAddr)
        {
            // Note: hash values may be 0 at this moment if the engine's
            // static init has not yet populated them; the addresses
            // themselves are correct and the hook callback re-reads on
            // every fire.
            DMK::Logger::get_instance().info(
                "ControlledChar: focus-actor hash global addresses "
                "published (kliff=0x{:X}, oongka=0x{:X}, damian=0x{:X})",
                static_cast<std::uint64_t>(kliffAddr),
                static_cast<std::uint64_t>(oongkaAddr),
                static_cast<std::uint64_t>(damianAddr));
        }
    }

    bool resolve_and_publish_focus_actor_globals() noexcept
    {
        constexpr std::string_view k_kliffName  = "focus-actor-kliff";
        constexpr std::string_view k_oongkaName = "focus-actor-oongka";
        constexpr std::string_view k_damianName = "focus-actor-damian";

        auto compiled = DetourModKit::Scanner::parse_aob(
            CDCore::Anchors::k_focusActorInitPattern);
        if (!compiled)
        {
            DMK::Logger::get_instance().warning(
                "ControlledChar: focus-actor init pattern failed to "
                "compile -- Tier-0 disabled");
            return false;
        }

        // Resolve the main module's base + size so we can scan the
        // whole image (including the `.tls` section). The bridge
        // functions live in `.tls` on this binary; that section is
        // mapped without PAGE_EXECUTE so scan_executable_regions
        // skips it. Module-bounded find_pattern covers any section
        // regardless of execute permission.
        const auto mainModule = ::GetModuleHandleW(nullptr);
        if (mainModule == nullptr)
        {
            DMK::Logger::get_instance().warning(
                "ControlledChar: GetModuleHandle(nullptr) returned NULL "
                "-- focus-actor AOB scan aborted");
            return false;
        }
        MODULEINFO modInfo{};
        if (!::GetModuleInformation(::GetCurrentProcess(),
                                    mainModule,
                                    &modInfo,
                                    sizeof(modInfo)))
        {
            DMK::Logger::get_instance().warning(
                "ControlledChar: GetModuleInformation failed (gle={}) "
                "-- focus-actor AOB scan aborted",
                ::GetLastError());
            return false;
        }
        const auto *moduleBase =
            reinterpret_cast<const std::byte *>(modInfo.lpBaseOfDll);
        const auto moduleSize =
            static_cast<std::size_t>(modInfo.SizeOfImage);

        std::uintptr_t kliffGlobal  = 0;
        std::uintptr_t oongkaGlobal = 0;
        std::uintptr_t damianGlobal = 0;

        // The pattern matches the generic "register tag via
        // sub_140F46680" bridge shape used by HUNDREDS of engine
        // strings (focus-actor-*, layer tags, query keys, etc.), not
        // just the three protagonists. Walk the entire image with an
        // advancing start pointer so the total cost stays O(image)
        // rather than O(N * image), and dispatch each hit by its
        // string content. The loop exits early once all three
        // protagonists are bound.
        const std::byte *cursor      = moduleBase;
        const auto      *moduleEnd   = moduleBase + moduleSize;
        constexpr std::size_t k_maxScannedHits = 4096;
        std::size_t hitsScanned = 0;
        while (cursor < moduleEnd && hitsScanned < k_maxScannedHits)
        {
            const auto remaining =
                static_cast<std::size_t>(moduleEnd - cursor);
            const auto *match =
                DetourModKit::Scanner::find_pattern(
                    cursor, remaining, *compiled);
            if (match == nullptr)
                break;
            ++hitsScanned;
            cursor = match + 1;

            const auto matchAddr =
                reinterpret_cast<std::uintptr_t>(match);

            // Resolve the string LEA: disp32 at +9, RIP after instr at +13.
            const auto strDispAddr =
                matchAddr + CDCore::Anchors::k_focusActorInitStringDispOffset;
            std::int32_t strDisp = 0;
            std::memcpy(&strDisp,
                        reinterpret_cast<const void *>(strDispAddr),
                        sizeof(strDisp));
            const auto strAddr =
                matchAddr +
                CDCore::Anchors::k_focusActorInitStringInstrEndOffset +
                static_cast<std::ptrdiff_t>(strDisp);

            char        nameBuf[32];
            std::size_t nameLen = 0;
            if (!seh_read_cstring(strAddr, nameBuf, sizeof(nameBuf), nameLen))
                continue;
            std::string_view name{nameBuf, nameLen};

            // Resolve the dword LEA: disp32 at +22, RIP after instr at +26.
            const auto dwordDispAddr =
                matchAddr + CDCore::Anchors::k_focusActorInitDwordDispOffset;
            std::int32_t dwordDisp = 0;
            std::memcpy(&dwordDisp,
                        reinterpret_cast<const void *>(dwordDispAddr),
                        sizeof(dwordDisp));
            const auto dwordAddr =
                matchAddr +
                CDCore::Anchors::k_focusActorInitDwordInstrEndOffset +
                static_cast<std::ptrdiff_t>(dwordDisp);

            if (name == k_kliffName)
                kliffGlobal = dwordAddr;
            else if (name == k_oongkaName)
                oongkaGlobal = dwordAddr;
            else if (name == k_damianName)
                damianGlobal = dwordAddr;

            if (kliffGlobal && oongkaGlobal && damianGlobal)
                break;
        }

        if (!kliffGlobal || !oongkaGlobal || !damianGlobal)
        {
            DMK::Logger::get_instance().warning(
                "ControlledChar: focus-actor AOB resolved {} of 3 "
                "globals (kliff=0x{:X} oongka=0x{:X} damian=0x{:X}) "
                "-- Tier-0 disabled",
                (kliffGlobal ? 1 : 0) +
                    (oongkaGlobal ? 1 : 0) +
                    (damianGlobal ? 1 : 0),
                static_cast<std::uint64_t>(kliffGlobal),
                static_cast<std::uint64_t>(oongkaGlobal),
                static_cast<std::uint64_t>(damianGlobal));
            return false;
        }

        set_focus_actor_hash_globals(kliffGlobal, oongkaGlobal, damianGlobal);
        return true;
    }

    ControlledCharacter current_controlled_character() noexcept
    {
        // Five-layer resolve. See the file-level header for the why
        // behind each ordering choice (in particular: why Layer 2
        // gates on non-Kliff, and why Layer 3 must outlive
        // invalidate_controlled_character()).
        const auto controlledBody = walk_to_controlled_body_seh();

        // Layer 1: structural Kliff anchor.
        if (body_is_structurally_kliff(controlledBody))
        {
            s_lastGoodChar.store(
                static_cast<std::uint8_t>(ControlledCharacter::Kliff),
                std::memory_order_release);
            stamp_body_cache(controlledBody,
                             ControlledCharacter::Kliff);
            return ControlledCharacter::Kliff;
        }

        // Layer 2: Tier-0 if non-Kliff (bypasses stale Kliff
        // intermediate broadcast).
        const auto focusCh = static_cast<ControlledCharacter>(
            s_focusBroadcastChar.load(std::memory_order_acquire));
        if (focusCh == ControlledCharacter::Damiane ||
            focusCh == ControlledCharacter::Oongka)
        {
            stamp_body_cache(controlledBody, focusCh);
            return focusCh;
        }

        // Layer 3: last-known non-Kliff cache (saves the same-non-
        // Kliff save-reload case).
        const auto lastNonKliff = static_cast<ControlledCharacter>(
            s_lastKnownNonKliff.load(std::memory_order_acquire));
        if (lastNonKliff != ControlledCharacter::Unknown)
        {
            stamp_body_cache(controlledBody, lastNonKliff);
            return lastNonKliff;
        }

        // Layer 4: Tier-0 even if Kliff.
        if (focusCh != ControlledCharacter::Unknown)
        {
            stamp_body_cache(controlledBody, focusCh);
            return focusCh;
        }

        // Layer 5: LKG.
        return static_cast<ControlledCharacter>(
            s_lastGoodChar.load(std::memory_order_acquire));
    }

    bool install_focus_broadcast_hook(std::uintptr_t hookAddr) noexcept
    {
        std::lock_guard<std::mutex> lk(s_focusHookMutex);

        const auto current =
            s_focusBroadcastHookAddr.load(std::memory_order_acquire);

        if (hookAddr == 0)
        {
            if (current == 0)
                return true;
            s_focusBroadcastHook = safetyhook::MidHook{};
            s_focusBroadcastHookAddr.store(0, std::memory_order_release);
            DMK::Logger::get_instance().info(
                "ControlledChar: focus-broadcast hook uninstalled");
            return true;
        }

        if (hookAddr < k_minValidPtr)
        {
            DMK::Logger::get_instance().warning(
                "ControlledChar: focus-broadcast hook rejected -- "
                "address 0x{:X} below guard region",
                static_cast<std::uint64_t>(hookAddr));
            return false;
        }

        if (current == hookAddr)
            return true;

        if (current != 0)
        {
            DMK::Logger::get_instance().info(
                "ControlledChar: focus-broadcast hook target shifted "
                "0x{:X} -> 0x{:X}, rebuilding",
                static_cast<std::uint64_t>(current),
                static_cast<std::uint64_t>(hookAddr));
            s_focusBroadcastHook = safetyhook::MidHook{};
            s_focusBroadcastHookAddr.store(0, std::memory_order_release);
        }

        auto built = safetyhook::MidHook::create(
            reinterpret_cast<void *>(hookAddr),
            focus_broadcast_midhook);
        if (!built)
        {
            DMK::Logger::get_instance().warning(
                "ControlledChar: focus-broadcast hook creation failed "
                "at 0x{:X}",
                static_cast<std::uint64_t>(hookAddr));
            return false;
        }

        s_focusBroadcastHook = std::move(*built);
        s_focusBroadcastHookAddr.store(hookAddr, std::memory_order_release);

        DMK::Logger::get_instance().info(
            "ControlledChar: focus-broadcast hook installed at 0x{:X}",
            static_cast<std::uint64_t>(hookAddr));
        return true;
    }

    void uninstall_focus_broadcast_hook() noexcept
    {
        (void)install_focus_broadcast_hook(0);
    }

    bool install_radial_swap_hook(std::uintptr_t hookAddr) noexcept
    {
        std::lock_guard<std::mutex> lk(s_hookMutex);

        const auto current =
            s_radialSwapHookAddr.load(std::memory_order_acquire);

        if (hookAddr == 0)
        {
            if (current == 0)
                return true;
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
            return true;

        if (current != 0)
        {
            DMK::Logger::get_instance().info(
                "ControlledChar: radial-swap hook target shifted "
                "0x{:X} -> 0x{:X}, rebuilding",
                static_cast<std::uint64_t>(current),
                static_cast<std::uint64_t>(hookAddr));
            s_radialSwapHook = safetyhook::MidHook{};
            s_radialSwapHookAddr.store(0, std::memory_order_release);
        }

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
            case ControlledCharacter::Oongka:  return "Oongka";
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
            case ControlledCharacter::Oongka:  return 3;
            case ControlledCharacter::Unknown: return 0;
        }
        return 0;
    }

    std::uint32_t resolve_character_idx_for_body(
        std::uintptr_t body) noexcept
    {
        if (body < k_minValidPtr)
            return 0;
        std::shared_lock<std::shared_mutex> lk(s_bodyCacheMutex);
        const auto it = s_bodyKeyCache.find(body);
        if (it == s_bodyKeyCache.end())
            return 0;
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
            return 0;
        std::shared_lock<std::shared_mutex> lk(s_bodyCacheMutex);
        std::size_t written = 0;
        for (const auto &kv : s_bodyKeyCache)
        {
            if (written >= cap)
                break;
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

    bool radial_swap_pending() noexcept
    {
        const auto t =
            s_radialSwapTimestampMs.load(std::memory_order_acquire);
        if (t == 0)
            return false;
        const auto now = GetTickCount64();
        return (now >= t) && ((now - t) <= k_radialSwapPendingWindowMs);
    }

    void invalidate_swap_caches() noexcept
    {
        // Swap-scope invalidation: clear ONLY the radial-swap
        // timestamp. Tier-0 (`s_focusBroadcastChar`) and the LKG
        // cache are deliberately left intact -- they self-update on
        // every focus broadcast, and a manual clear here would open
        // an Unknown window between the swap-detect callback and the
        // next resolver query that the resolver has no pull-mode
        // fallback for.
        //
        // The timestamp itself must be cleared because consumer
        // save-load disambiguation (`radial_swap_pending()`) must
        // not false-positive across a save-load that immediately
        // follows a radial swap.
        s_radialSwapTimestampMs.store(0, std::memory_order_release);
    }

    void invalidate_controlled_character() noexcept
    {
        // Full world-reload clear. Save-load transitions reallocate
        // every chain pointer including the body pool; flushing the
        // body cache here prevents post-load mis-attribution where the
        // engine reuses a previously-cached body address for a
        // different protagonist.
        //
        // Unlike invalidate_swap_caches(), this DOES clear Tier-0 +
        // LKG because the engine is about to fire a fresh focus
        // broadcast for the new world's controlled character; the
        // brief Unknown window is intentional and bounded by the
        // engine's world-spawn focus event (typically <1 s).
        s_focusBroadcastChar.store(
            static_cast<std::uint8_t>(ControlledCharacter::Unknown),
            std::memory_order_release);
        s_lastGoodChar.store(
            static_cast<std::uint8_t>(ControlledCharacter::Unknown),
            std::memory_order_release);
        s_radialSwapTimestampMs.store(0, std::memory_order_release);

        // `s_lastKnownNonKliff` is deliberately PRESERVED. It is
        // the only signal that recovers identity on a same-non-
        // Kliff save reload, where the engine fires an intermediate
        // Kliff broadcast and never re-broadcasts the actual
        // Damiane/Oongka body.

        {
            std::unique_lock<std::shared_mutex> lk(s_bodyCacheMutex);
            s_bodyKeyCache.clear();
        }
    }

} // namespace CDCore
