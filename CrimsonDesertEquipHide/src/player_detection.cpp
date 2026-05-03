#include "player_detection.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>

namespace EquipHide
{
    static std::atomic<uint32_t> s_resolveCounter{0};
    static constexpr uint32_t k_resolveInterval = 512;

    static uintptr_t s_prevVisCtrls[k_maxProtagonists]{};
    static int s_prevCount = 0;

    /* Per-body vc cache. v1.05 zeroes body+0x68 (and downstream) for
       inactive protagonists, so the live chain walk only succeeds
       for the currently-controlled body. The vis_ctrl pointer itself
       stays valid across swaps within a session, so caching the last
       successful resolution per body lets the resolver publish all
       three protagonists' vcs after the user has cycled through them
       once. Across save-load the engine reallocates its arenas: the
       cached vcs are dangling and MUST be wiped or DirectWrite stomps
       freed memory. File-scope so resolve_player_vis_ctrls can clear
       it on the save-load transition. */
    struct Body2VcEntry
    {
        uintptr_t body;
        uintptr_t vc;
    };
    static Body2VcEntry s_body2vcLru[k_maxProtagonists]{};
    static int s_body2vcNext = 0;

    static void clear_body_to_vis_ctrl_cache() noexcept
    {
        for (auto &e : s_body2vcLru)
            e = {0, 0};
        s_body2vcNext = 0;
    }

    /* Full save-load wipe shared by the X->0->Y deferred-reload
       state machine and the atomic-swap fallback (where the engine
       rotates user+0xD8 between two non-zero values without ever
       publishing the null window). Both paths land here once the
       resolver has decided "this is a new world, drop everything":
       Core's body learning cache, the per-body vc LRU, and the
       published player_state must all be cleared together so the
       next resolve cycle rebuilds against the fresh arena instead
       of stomping freed memory through stale vc pointers. */
    static void apply_full_reload_wipe() noexcept
    {
        CDCore::invalidate_controlled_character();
        clear_body_to_vis_ctrl_cache();
        auto &psWipe = player_state();
        for (int j = 0; j < k_maxProtagonists; ++j)
        {
            psWipe.visCtrls[j].store(0, std::memory_order_relaxed);
            psWipe.visCharIdx[j].store(-1, std::memory_order_relaxed);
            psWipe.armorInjected[j].store(false, std::memory_order_relaxed);
        }
        psWipe.primaryVisCtrl.store(0, std::memory_order_relaxed);
        psWipe.count.store(0, std::memory_order_relaxed);
        for (int j = 0; j < k_maxProtagonists; ++j)
            s_prevVisCtrls[j] = 0;
        s_prevCount = 0;
    }

    /** @brief Traverse body -> vis_ctrl pointer chain. Caller MUST be SEH-protected.
     *  @details Trace-logs only when a (body -> vc) mapping is new or has
     *           changed since the last walk; successful walks with a
     *           previously-seen mapping are silent. The resolver fires
     *           hundreds of times per second and a full trace on every call
     *           floods the log. Chain-broken paths always log since those
     *           indicate real state transitions worth seeing. */
    static uintptr_t body_to_vis_ctrl(uintptr_t body) noexcept
    {
        if (!body)
            return 0;

        auto inner = read_ptr_unsafe(body, 0x68);
        if (!inner)
        {
            for (const auto &e : s_body2vcLru)
            {
                if (e.body == body && e.vc != 0)
                    return e.vc;
            }
            DMK::Logger::get_instance().trace(
                "body_to_vis_ctrl: body=0x{:X} inner=NULL (+0x68), no cache",
                body);
            return 0;
        }
        auto sub = read_ptr_unsafe(inner, 0x40);
        if (!sub)
        {
            for (const auto &e : s_body2vcLru)
            {
                if (e.body == body && e.vc != 0)
                    return e.vc;
            }
            DMK::Logger::get_instance().trace(
                "body_to_vis_ctrl: body=0x{:X} inner=0x{:X} sub=NULL (+0x40), "
                "no cache", body, inner);
            return 0;
        }
        auto vc = read_ptr_unsafe(sub, 0xE8);

        // Update LRU on success and dedupe trace logging.
        for (auto &e : s_body2vcLru)
        {
            if (e.body == body && e.vc == vc)
                return vc;
        }
        s_body2vcLru[s_body2vcNext] = {body, vc};
        s_body2vcNext = (s_body2vcNext + 1) % k_maxProtagonists;

        DMK::Logger::get_instance().trace(
            "body_to_vis_ctrl: body=0x{:X} inner=0x{:X} sub=0x{:X} vc=0x{:X}",
            body, inner, sub, vc);
        return vc;
    }

    void resolve_player_vis_ctrls() noexcept
    {
        auto &addrs = resolved_addrs();
        if (!addrs.worldSystem || !addrs.childActorVtbl)
            return;

        auto &ps = player_state();

        __try
        {
            /* read_ptr_unsafe: outer __try makes is_readable() redundant. */
            auto ws = read_ptr_unsafe(addrs.worldSystem, 0);
            if (!ws)
                return;
            auto am = read_ptr_unsafe(ws, 0x30);
            if (!am)
                return;

            auto user = read_ptr_unsafe(am, 0x28);
            if (!user)
                return;

            /* World-reload invalidation. The UserActor pointer is
               reallocated on every save load (the ActorManager rewrites
               it; in-session character swaps only rotate user+0xD8 and
               leave the singleton itself alone). Comparing the most
               recently-seen user against the freshly-walked one is
               therefore a reliable signal for "new world": when it
               flips, wipe the Core resolver's cache so it does not
               keep returning the previous save's character while the
               new save is still populating the engine state. Runs
               independently of LiveTransmog so EquipHide is self-
               contained when loaded standalone.

               The prevUser != 0 inner guard suppresses first-boot
               invalidation: on the very first tick prevUser is zero
               and curUser is the first valid singleton; invalidating
               here would wipe a cache the resolver may have just
               populated against the initial world. Static file scope
               is fine because player_detection is single-threaded
               (driven by the background_threads tick). */
            static std::uintptr_t s_prevUser = 0;
            if (user != s_prevUser)
            {
                if (s_prevUser != 0)
                {
                    DMK::Logger::get_instance().info(
                        "Load detect: UserActor swapped "
                        "(0x{:X} -> 0x{:X}); invalidating controlled-"
                        "char cache for save-load transition",
                        s_prevUser, user);
                    CDCore::invalidate_controlled_character();
                }
                s_prevUser = user;
            }

            /* Character-swap / save-load invalidation, split by
               transition type:

               1. controlledActor X -> 0  (entering load screen). v1.05
                  preserves the UserActor singleton across save-load,
                  so the s_prevUser branch above does NOT fire. The
                  only reliable signal that the world is being torn
                  down is user+0xD8 going NULL. We mark a deferred
                  reload; the actual cache wipe happens on the
                  matching 0 -> Y transition because flushing while
                  the engine is mid-teardown can race with bodies
                  still in flight. CDCore::invalidate_controlled_
                  character() and the body_to_vis_ctrl LRU clear
                  together drop every dangling vc pointer that would
                  otherwise alias into the next save's freshly
                  reallocated arena -- DirectWrite stomping a freed
                  vc was the v1.05 post-load regression.

               2. controlledActor 0 -> Y  (load screen -> new save).
                  Drain the deferred reload flag: full invalidation +
                  LRU wipe so the next resolve cycle rebuilds from
                  scratch against the new world's bodies.

               3. controlledActor X -> Y, both non-zero  (in-session
                  Kliff <-> Damiane <-> Oongka swap). Bodies are
                  reused from the pool and the body learning cache
                  populated by CDCore stays valid across the swap;
                  flushing it would force every party member to fall
                  back to the active character's hide mask until the
                  user manually cycled through each protagonist
                  again. Only evict the swap-scope state here: the
                  LKG identity (so the resolver does not return the
                  OLD character on the first post-swap query before
                  the radial hook / slot decode catches up) and the
                  actor->character cache.

               First-boot suppression (s_prevControlledActor != 0)
               guards transitions where the prior value is sentinel
               zero and the freshly-walked pointer is the initial
               controlled actor, so invalidating would wipe a cache
               just populated against it. */
            auto controlledActor = read_ptr_unsafe(user, 0xD8);
            static std::uintptr_t s_prevControlledActor = 0;
            static bool s_pendingReloadInvalidation = false;
            if (controlledActor != s_prevControlledActor)
            {
                if (controlledActor == 0 && s_prevControlledActor != 0)
                {
                    DMK::Logger::get_instance().info(
                        "Save-load detected: controlled actor "
                        "(0x{:X} -> 0x0); deferring full cache wipe "
                        "until new world is live",
                        s_prevControlledActor);
                    s_pendingReloadInvalidation = true;
                }
                else if (controlledActor != 0 && s_prevControlledActor == 0 &&
                         s_pendingReloadInvalidation)
                {
                    DMK::Logger::get_instance().info(
                        "Save-load complete: new controlled actor 0x{:X}; "
                        "wiping body cache + body_to_vis_ctrl LRU",
                        controlledActor);
                    apply_full_reload_wipe();
                    s_pendingReloadInvalidation = false;
                }
                else if (s_prevControlledActor != 0 && controlledActor != 0)
                {
                    /* Atomic-swap save-load: v1.05 sometimes rotates
                       user+0xD8 directly between two non-zero values
                       (load-screen window shorter than the resolver's
                       1 s poll, or the engine builds the new actor
                       and atomically swaps without ever publishing
                       null) which defeats the X->0->Y state machine
                       above and would otherwise mis-classify as an
                       in-session radial swap. Disambiguate via the
                       Core body learning cache: every protagonist
                       body that was ever observed as the controlled
                       identity this session is cache-resident, so an
                       arriving controlled actor that matches NONE of
                       the cache's entries cannot be one of the bodies
                       we have already learned. CDCore is statically
                       linked into each Logic DLL, so this cache is
                       EH-private (no race with LiveTransmog stamping
                       its own copy first) and is only mutated by the
                       resolver call further down in this function.
                       Disambiguator: a real radial swap leaves a fresh
                       pending-key record stamped by the radial-swap
                       safetyhook callback (TTL ~2 s, consumed by the
                       next chain walk). A save-load has no such record
                       because
                       the load originates from the pause/title menu,
                       not the radial UI. So if the new actor is absent
                       from the body cache AND no radial input was just
                       observed, the rotation must be a new-arena
                       allocation -- treat as save-load. The cache-size
                       lower bound (>= 1) guards the immediate post-
                       wipe / pre-stamp window where peekN == 0 carries
                       no information. */
                    std::array<CDCore::BodyCacheEntry, 3> peek;
                    const auto peekN = CDCore::snapshot_body_cache(
                        peek.data(), peek.size());
                    bool inCache = false;
                    for (std::size_t i = 0; i < peekN; ++i)
                    {
                        if (peek[i].body == controlledActor)
                        {
                            inCache = true;
                            break;
                        }
                    }
                    const bool atomicSaveLoad =
                        peekN >= 1 && !inCache &&
                        !CDCore::radial_swap_pending();

                    if (atomicSaveLoad)
                    {
                        DMK::Logger::get_instance().info(
                            "Save-load detected (atomic swap): "
                            "controlled actor (0x{:X} -> 0x{:X}) "
                            "absent from body cache (n={}, no pending "
                            "radial); wiping body cache + "
                            "body_to_vis_ctrl LRU",
                            s_prevControlledActor, controlledActor,
                            peekN);
                        apply_full_reload_wipe();
                        /* No X->0 was observed, so the deferred flag
                           was never latched -- defensively clear so a
                           later spurious X->0->Y cannot double-fire
                           against this transition. */
                        s_pendingReloadInvalidation = false;
                    }
                    else
                    {
                        DMK::Logger::get_instance().info(
                            "Char swap detected: controlled actor "
                            "(0x{:X} -> 0x{:X}); invalidating swap-scope "
                            "caches (body cache preserved)",
                            s_prevControlledActor, controlledActor);
                        CDCore::invalidate_swap_caches();
                    }
                }
                s_prevControlledActor = controlledActor;
            }

            /* Bail until the new world has a controlled actor. With
               s_pendingReloadInvalidation still set, walking the body
               cache against the next save's bodies before the wipe
               re-publishes the previous save's vcs (and DirectWrite
               then stomps freed memory). */
            if (controlledActor == 0)
                return;

            /* Controlled-character identity via the shared Core
               resolver. Core walks WorldSystem -> ActorManager ->
               UserActor -> controlled_actor(+0xD8), then decodes the
               (party_class, char_kind) u32 pair at the actor's +0xDC
               and +0xEC into a known character. The WorldSystem holder
               address is published by this module's init in
               equip_hide.cpp via CDCore::set_world_system_holder, so
               the resolver is wired up by the time the polling loop
               first reaches this point. Idempotent across consumers:
               LiveTransmog publishes the same address and the later
               writer wins. Resolver returns 0 -> idx=-1 -> disabled
               override on holder-not-published, faulted chain walk,
               or unknown identity key. */
            {
                const auto idxU32 = CDCore::current_controlled_character_idx();
                const int idx = (idxU32 >= 1 && idxU32 <= 3)
                                    ? static_cast<int>(idxU32) - 1
                                    : -1;
                set_active_character(idx);
            }

            /* Build the protagonist vis-ctrl list strictly from the body
               learning cache. Each entry there was stamped while the
               resolver observed a known controlled identity, so it is
               guaranteed to be a protagonist body and never an NPC
               sharing the child-actor vtable. Bodies the user has not
               yet been in this session are absent from the cache;
               consumers downstream of visCtrls fall back to vanilla
               visibility for those bodies, which is the user-accepted
               trade (one radial cycle through all three protagonists
               at session start populates every entry). */
            std::array<CDCore::BodyCacheEntry, k_maxProtagonists> bodyEntries;
            const auto entryCount = CDCore::snapshot_body_cache(
                bodyEntries.data(), bodyEntries.size());

            int count = 0;
            for (std::size_t i = 0; i < entryCount; ++i)
            {
                if (count >= k_maxProtagonists)
                {
                    break;
                }
                if (bodyEntries[i].charIdx < 1 ||
                    bodyEntries[i].charIdx > 3)
                {
                    continue;
                }
                /* body_to_vis_ctrl is SEH-protected on its inner reads
                   but the outer __try wrapping the whole resolve
                   already covers the chase here. */
                const auto vc = body_to_vis_ctrl(bodyEntries[i].body);
                if (!vc)
                {
                    continue;
                }

                /* Dedupe against earlier entries -- a single body
                   cannot map to more than one vis-ctrl in practice,
                   but the dedup guards against a torn-write window
                   between the body stamp and the body_to_vis_ctrl
                   walk inside this same resolve cycle. */
                bool dup = false;
                for (int k = 0; k < count; ++k)
                {
                    if (ps.visCtrls[k].load(std::memory_order_relaxed) == vc)
                    {
                        dup = true;
                        break;
                    }
                }
                if (dup)
                {
                    continue;
                }

                const int charIdx = static_cast<int>(bodyEntries[i].charIdx) - 1;
                ps.visCtrls[count].store(vc, std::memory_order_relaxed);
                ps.visCharIdx[count].store(charIdx, std::memory_order_relaxed);
                ++count;
            }

            for (int i = count; i < k_maxProtagonists; ++i)
            {
                ps.visCtrls[i].store(0, std::memory_order_relaxed);
                ps.visCharIdx[i].store(-1, std::memory_order_relaxed);
            }

            ps.primaryVisCtrl.store(
                count > 0 ? ps.visCtrls[0].load(std::memory_order_relaxed) : 0,
                std::memory_order_relaxed);

            for (int i = 0; i < k_maxProtagonists; ++i)
                ps.armorInjected[i].store(false, std::memory_order_relaxed);

            ps.count.store(count, std::memory_order_relaxed);

            {
                static std::atomic<bool> s_logged{false};
                if (!s_logged.exchange(true, std::memory_order_relaxed))
                    DMK::Logger::get_instance().debug(
                        "Resolve: ws=0x{:X} am=0x{:X} user=0x{:X} count={}",
                        ws, am, user, count);
            }
            if (count > 0)
            {
                static std::atomic<bool> s_resolvedLogged{false};
                if (!s_resolvedLogged.exchange(true, std::memory_order_relaxed))
                    DMK::Logger::get_instance().info(
                        "Player set resolved: {} protagonist(s) tracked",
                        count);
            }
            if (count > 0)
            {
                /* Non-blocking: skip if the input thread holds the mutex;
                   the next resolve cycle will catch any change. */
                auto &mtx = vis_write_mutex();
                if (mtx.try_lock())
                {
                    bool changed = (count != s_prevCount);
                    if (!changed)
                    {
                        for (int j = 0; j < count; ++j)
                        {
                            if (ps.visCtrls[j].load(std::memory_order_relaxed) != s_prevVisCtrls[j])
                            {
                                changed = true;
                                break;
                            }
                        }
                    }
                    if (changed)
                    {
                        s_prevCount = count;
                        for (int j = 0; j < count; ++j)
                            s_prevVisCtrls[j] = ps.visCtrls[j].load(std::memory_order_relaxed);
                        for (int j = 0; j < k_maxProtagonists; ++j)
                            ps.armorInjected[j].store(false, std::memory_order_relaxed);
                        needs_direct_write().store(true, std::memory_order_relaxed);
                        DMK::Logger::get_instance().debug(
                            "Player set changed -- scheduling injection + direct write");
                    }
                    mtx.unlock();
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                DMK::Logger::get_instance().warning("Resolve: SEH caught crash");
        }
    }

    bool is_player_vis_ctrl(uintptr_t a1) noexcept
    {
        auto &ps = player_state();
        if (a1 == ps.primaryVisCtrl.load(std::memory_order_relaxed))
            return true;

        const auto n = ps.count.load(std::memory_order_relaxed);
        for (int i = 1; i < n; ++i)
        {
            if (ps.visCtrls[i].load(std::memory_order_relaxed) == a1)
                return true;
        }
        return false;
    }

    bool check_player_filter(uintptr_t a1) noexcept
    {
        if (a1 < 0x10000)
            return false;

        auto &ps = player_state();

        if (flag_fallback_mode().load(std::memory_order_relaxed))
        {
            /* Actor type byte: *(*(actor+0x88)+1). Value 1 = local player,
               3-6 = party members. Same mechanism the headgear visibility
               system uses. */
            auto comp = read_ptr_unsafe(a1, 0x58);
            if (comp)
            {
                auto actor = read_ptr_unsafe(comp, 0x08);
                if (actor)
                {
                    auto typePtr = read_ptr_unsafe(actor, 0x88);
                    if (typePtr)
                    {
                        auto typeByte = *reinterpret_cast<const uint8_t *>(typePtr + 1);
                        {
                            static std::atomic<int> s_fbLog{0};
                            if (s_fbLog.fetch_add(1, std::memory_order_relaxed) < 5)
                                DMK::Logger::get_instance().trace(
                                    "Fallback chain: a1=0x{:X} comp=0x{:X} "
                                    "actor=0x{:X} typePtr=0x{:X} type={}",
                                    a1, comp, actor, typePtr, typeByte);
                        }
                        bool isProtagonist = (typeByte == 1) ||
                                             (typeByte >= 3 && typeByte <= 6);
                        if (isProtagonist)
                        {
                            const auto n = ps.count.load(std::memory_order_relaxed);
                            bool alreadyCached = false;
                            for (int i = 0; i < n; ++i)
                            {
                                if (ps.visCtrls[i].load(std::memory_order_relaxed) == a1)
                                {
                                    alreadyCached = true;
                                    break;
                                }
                            }
                            if (!alreadyCached && n < k_maxProtagonists)
                            {
                                int expected = n;
                                if (ps.count.compare_exchange_weak(
                                        expected, n + 1, std::memory_order_relaxed))
                                {
                                    ps.visCtrls[n].store(a1, std::memory_order_relaxed);
                                    /* Fallback path runs when the global
                                       chain-walk AOB failed at init, so
                                       the body pointer underlying `a1`
                                       is not directly reachable -- mark
                                       the slot's identity as unknown.
                                       Consumers fall back to the active
                                       character's hide mask, mirroring
                                       single-character semantics for
                                       unidentified slots. */
                                    ps.visCharIdx[n].store(-1, std::memory_order_relaxed);
                                    ps.primaryVisCtrl.store(
                                        ps.visCtrls[0].load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
                                    needs_direct_write().store(true, std::memory_order_relaxed);
                                    DMK::Logger::get_instance().debug(
                                        "Fallback: cached protagonist vis ctrl at slot {} "
                                        "(0x{:X}, type={})",
                                        n, a1, typeByte);
                                }
                            }
                        }
                    }
                }
            }

            /* Fail-closed: until the fallback path has cached at
               least one protagonist, do NOT admit the candidate. The
               historic permissive `<= 0` admitted every actor during
               the resolve gap, leaking hides onto NPCs that then
               could not be restored at runtime (their vis ctrls were
               never in the active set, so the orphan sweep skipped
               them). */
            return ps.count.load(std::memory_order_relaxed) > 0 &&
                   is_player_vis_ctrl(a1);
        }

        // Global pointer chain mode.
        auto cnt = s_resolveCounter.fetch_add(1, std::memory_order_relaxed);
        if (ps.count.load(std::memory_order_relaxed) == 0 ||
            (cnt & (k_resolveInterval - 1)) == 0)
            resolve_player_vis_ctrls();

        /* Fail-closed (see fallback branch above for rationale). */
        return ps.count.load(std::memory_order_relaxed) > 0 &&
               is_player_vis_ctrl(a1);
    }

} // namespace EquipHide
