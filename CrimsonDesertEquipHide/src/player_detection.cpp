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
    /**
     * @brief Minimum gap between inline (game-thread) resolves while no protagonist is known (ms).
     * @details A save-load wipe zeroes the published player count. Without a floor every visibility check in that
     *          window starts its own actor sweep. 250ms stays responsive against the resolve-poll thread's 1s tick
     *          and bounds the game thread to four sweeps a second.
     */
    static constexpr int64_t k_inlineResolveMinIntervalMs = 250;

    /** @brief Timestamp of the last inline resolve, in @ref steady_ms units. Zero means none has run. */
    static std::atomic<int64_t> s_lastInlineResolveMs{0};

    static uintptr_t s_prevVisCtrls[k_maxProtagonists]{};
    static int s_prevCount = 0;

    // Per-body vc cache. The engine zeroes body+0x68 and everything downstream for an inactive protagonist, so the
    // live chain walk succeeds only for the currently-controlled body. The vis_ctrl pointer itself stays valid across
    // swaps within a session. A cache of the last successful resolution per body therefore lets the resolver publish
    // all three protagonists' vcs once the user cycles through them. Across save-load the engine reallocates its
    // arenas, the cached vcs dangle, and the cache MUST be wiped or DirectWrite stomps freed memory. File scope lets
    // resolve_player_vis_ctrls clear it on the save-load transition.
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

    // Full save-load wipe, shared by the X->0->Y deferred-reload state machine and by the atomic-swap fallback,
    // where the engine rotates user+0xD8 between two non-zero values and never publishes the null window. Both paths
    // land here once the resolver decides the world is new and everything drops. Core's body learning cache, the
    // per-body vc LRU and the published player_state clear together, so the next resolve cycle rebuilds against the
    // fresh arena instead of a stomp through a stale vc pointer.
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

    /**
     * @brief Traverse body -> vis_ctrl pointer chain. Caller MUST be SEH-protected.
     * @details It trace-logs only a (body -> vc) mapping that is new or differs from the last walk. A successful walk
     *          on a previously-seen mapping stays silent. The resolver fires hundreds of times per second and a full
     *          trace on every call floods the log. A chain-broken path always logs, because that marks a real state
     *          transition worth seeing.
     */
    static uintptr_t body_to_vis_ctrl(uintptr_t body) noexcept
    {
        if (!body)
            return 0;

        // body -> +0x68 (inner) -> +0x40 (sub) -> *(sub+0xE8) (vc). The chain walk dereferences each link and reads
        // the terminal vc value under a single fault guard. A broken link falls through to the per-body LRU.
        const auto vcOpt = DMK::memory::walk(DMK::Address{body}, std::array<std::ptrdiff_t, 3>{0x68, 0x40, 0xE8})
                               .and_then([](DMK::Address leaf) { return DMK::memory::read<std::uintptr_t>(leaf); });
        if (!vcOpt)
        {
            for (const auto &e : s_body2vcLru)
            {
                if (e.body == body && e.vc != 0)
                    return e.vc;
            }
            DMK::log().trace("body_to_vis_ctrl: body=0x{:X} chain broken, no cache", body);
            return 0;
        }
        const auto vc = *vcOpt;

        // Update LRU on success and dedupe trace logging.
        for (auto &e : s_body2vcLru)
        {
            if (e.body == body && e.vc == vc)
                return vc;
        }
        s_body2vcLru[s_body2vcNext] = {body, vc};
        s_body2vcNext = (s_body2vcNext + 1) % k_maxProtagonists;

        DMK::log().trace("body_to_vis_ctrl: body=0x{:X} vc=0x{:X}", body, vc);
        return vc;
    }

    void resolve_player_vis_ctrls() noexcept
    {
        auto &addrs = resolved_addrs();
        if (!addrs.worldSystem || !addrs.childActorVtbl)
            return;

        auto &ps = player_state();

        // CDCore::ActorChainOffsets (controlled_char.hpp) owns the WorldSystem -> ActorManager -> UserActor chain
        // offsets. It is the single authority shared with background_threads and with CDCore's own resolver, so a
        // manager re-layout is a one-line edit there. This walk reaches the SAME pa::ClientActorManager that CDCore
        // roots at the published global slot.
        namespace AC = CDCore::ActorChainOffsets;

        __try
        {
            // read_ptr_unsafe: the outer __try makes an is_readable() pre-check redundant.
            auto ws = read_ptr_unsafe(addrs.worldSystem, 0);
            if (!ws)
                return;
            auto am = read_ptr_unsafe(ws, AC::k_worldSystemToActorManager);
            if (!am)
                return;

            auto user = read_ptr_unsafe(am, AC::k_actorManagerToUserActor);
            if (!user)
                return;

            // World-reload invalidation. The ActorManager reallocates the UserActor pointer on every save load. An
            // in-session character swap only rotates user+0xD8 and leaves the singleton alone. A compare of the most
            // recently seen user against the freshly walked one is therefore a reliable new-world signal. On a flip,
            // wipe the Core resolver's cache so it stops returning the previous save's character while the new save
            // still populates the engine state. It runs independently of LiveTransmog, so EquipHide is self-contained
            // when loaded standalone.
            //
            // The prevUser != 0 inner guard suppresses first-boot invalidation. On the very first tick prevUser is
            // zero and curUser is the first valid singleton, and a wipe there drops a cache the resolver just
            // populated against the initial world. Static file scope is safe because the background_threads tick
            // drives player_detection on one thread.
            static std::uintptr_t s_prevUser = 0;
            if (user != s_prevUser)
            {
                if (s_prevUser != 0)
                {
                    DMK::log().info(
                        "Load detect: UserActor swapped "
                        "(0x{:X} -> 0x{:X}); invalidating controlled-char cache for save-load transition",
                        s_prevUser,
                        user
                    );
                    CDCore::invalidate_controlled_character();
                }
                s_prevUser = user;
            }

            // Character-swap and save-load invalidation, driven by the X->0->Y transition of controlledActor. The
            // engine preserves the UserActor singleton across save-load, so the s_prevUser branch above does not fire
            // and a null user+0xD8 is the only reliable teardown signal. The X->0 edge marks a deferred reload and
            // the matching 0->Y edge performs the wipe, because a flush while the engine is mid-teardown races bodies
            // still in flight. CDCore::invalidate_controlled_character() and the body_to_vis_ctrl LRU clear together
            // so no dangling vc pointer aliases into the next save's freshly reallocated arena, which is what makes
            // DirectWrite stomp freed memory and regress visibility after a load.
            //
            // An X->Y edge with both values non-zero is an in-session Kliff, Damiane or Oongka swap. The engine
            // reuses bodies from the pool and CDCore's body learning cache stays valid, so only the swap-scope state
            // is evicted: the last-known-good identity and the actor-to-character cache. A full flush there forces
            // every party member onto the active character's hide mask until the user cycles each protagonist again.
            //
            // The s_prevControlledActor != 0 guard suppresses first boot, where the prior value is the sentinel zero
            // and the freshly walked pointer is the initial controlled actor.
            auto controlledActor = read_ptr_unsafe(user, AC::k_userActorToControlled);
            static std::uintptr_t s_prevControlledActor = 0;
            static bool s_pendingReloadInvalidation = false;
            // CDCore world-generation: bumps when the engine reallocates Kliff's CCOIA (save-load). Used by the
            // atomic-X->Y branch below to disambiguate a save-load from an in-session radial swap.
            //
            // An init of 0 is safe. The atomic-X->Y branch that consumes the prev value is gated on a non-zero
            // s_prevControlledActor, which also static-inits to 0, so it cannot fire on the very first tick. By tick
            // two the store below has overwritten s_prevWorldGen with curWorldGen.
            static std::uint64_t s_prevWorldGen = 0;
            const auto curWorldGen = CDCore::world_generation();
            if (controlledActor != s_prevControlledActor)
            {
                if (controlledActor == 0 && s_prevControlledActor != 0)
                {
                    DMK::log().info(
                        "Save-load detected: controlled actor "
                        "(0x{:X} -> 0x0); deferring full cache wipe until new world is live",
                        s_prevControlledActor
                    );
                    s_pendingReloadInvalidation = true;
                }
                else if (controlledActor != 0 && s_prevControlledActor == 0 && s_pendingReloadInvalidation)
                {
                    DMK::log().info(
                        "Save-load complete: new controlled actor 0x{:X}; wiping body cache + body_to_vis_ctrl LRU",
                        controlledActor
                    );
                    apply_full_reload_wipe();
                    s_pendingReloadInvalidation = false;
                }
                else if (s_prevControlledActor != 0 && controlledActor != 0)
                {
                    // Atomic-swap save-load: the engine rotates user+0xD8 directly between two non-zero values,
                    // either because the load-screen window is shorter than the resolver's 1s poll or because the
                    // engine builds the new actor and swaps atomically without ever publishing null. That defeats
                    // the X->0->Y state machine. CDCore::world_generation() disambiguates it: it increments when the
                    // engine reallocates Kliff's CCOIA during save-load. An in-session radial swap leaves Kliff's
                    // CCOIA pointer untouched and only flips sub-manager+0x38 between existing CCOIA pointers, so an
                    // unchanged generation marks a normal swap and the body_to_vis_ctrl LRU stays valid.
                    const bool atomicSaveLoad = curWorldGen != s_prevWorldGen;

                    if (atomicSaveLoad)
                    {
                        DMK::log().info(
                            "Save-load detected (atomic swap): "
                            "controlled actor (0x{:X} -> 0x{:X}); "
                            "world_generation {} -> {}; wiping body cache + body_to_vis_ctrl LRU",
                            s_prevControlledActor,
                            controlledActor,
                            s_prevWorldGen,
                            curWorldGen
                        );
                        apply_full_reload_wipe();
                        // No X->0 arrived, so the deferred flag never latched. Clear it defensively so a later
                        // spurious X->0->Y cannot double-fire against this transition.
                        s_pendingReloadInvalidation = false;
                    }
                    else
                    {
                        DMK::log().info(
                            "Char swap detected: controlled actor (0x{:X} -> 0x{:X}); body cache preserved",
                            s_prevControlledActor,
                            controlledActor
                        );
                    }
                }
                s_prevControlledActor = controlledActor;
            }
            s_prevWorldGen = curWorldGen;

            // Bail until the new world has a controlled actor. With s_pendingReloadInvalidation still set, a walk of
            // the body cache against the next save's bodies before the wipe re-publishes the previous save's vcs, and
            // DirectWrite then stomps freed memory.
            if (controlledActor == 0)
                return;

            // Controlled-character identity through the shared Core resolver. Core walks its own published chain
            // anchor to the controlled CCOIA, then classifies that CCOIA by its appearance-config asset path and
            // matches the embedded character codename. CDCore::ActorChainOffsets and the CDCore resolver own the
            // chain and its anchor, so nothing here needs a publisher or a hook. A torn chain read or an unknown
            // codename returns 0, which maps to idx=-1 and a disabled override.
            {
                const auto idxU32 = CDCore::current_controlled_character_idx();
                const int idx = (idxU32 >= 1 && idxU32 <= 3) ? static_cast<int>(idxU32) - 1 : -1;
                set_active_character(idx);
            }

            // Build the protagonist vis-ctrl list from the live player-CCOIA snapshot. Core walks its own chain.
            // Kliff is always present at the sub-manager slot. Damiane and Oongka come from the ClientActorManager's
            // CCOIA-only actor array and pass through the appearance-config classifier, which rejects the NPCs and
            // follower humanoids that share the array. The snapshot covers all three protagonists from frame zero,
            // so the user never has to cycle through them first.
            std::array<CDCore::BodyCacheEntry, k_maxProtagonists> bodyEntries;
            const auto entryCount = CDCore::snapshot_body_cache(bodyEntries.data(), bodyEntries.size());

            int count = 0;
            for (std::size_t i = 0; i < entryCount; ++i)
            {
                if (count >= k_maxProtagonists)
                {
                    break;
                }
                if (bodyEntries[i].charIdx < 1 || bodyEntries[i].charIdx > 3)
                {
                    continue;
                }
                // body_to_vis_ctrl guards its own inner reads, and the outer __try over the whole resolve already
                // covers this chase.
                const auto vc = body_to_vis_ctrl(bodyEntries[i].body);
                if (!vc)
                {
                    continue;
                }

                // Dedupe against an earlier entry. One body cannot map to more than one vis-ctrl in practice, but
                // the dedup guards a torn-write window between the body stamp and the body_to_vis_ctrl walk inside
                // this same resolve cycle.
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
                std::memory_order_relaxed
            );

            for (int i = 0; i < k_maxProtagonists; ++i)
                ps.armorInjected[i].store(false, std::memory_order_relaxed);

            ps.count.store(count, std::memory_order_relaxed);

            {
                static std::atomic<bool> s_logged{false};
                if (!s_logged.exchange(true, std::memory_order_relaxed))
                    DMK::log().debug("Resolve: ws=0x{:X} am=0x{:X} user=0x{:X} count={}", ws, am, user, count);
            }
            if (count > 0)
            {
                static std::atomic<bool> s_resolvedLogged{false};
                if (!s_resolvedLogged.exchange(true, std::memory_order_relaxed))
                    DMK::log().info("Player set resolved: {} protagonist(s) tracked", count);
            }
            if (count > 0)
            {
                // Non-blocking. Skip when the input thread holds the mutex. The next resolve cycle catches any
                // change.
                auto &mtx = vis_write_mutex();
                if (mtx.try_lock())
                {
                    // __try/__finally guarantees mtx.unlock() on every exit path. The outer __try/__except further
                    // below catches SEH from the inner body, and __finally covers the remaining exits, so the mutex
                    // is always released.
                    __try
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
                            needs_direct_write().store(true, std::memory_order_release);
                            DMK::log().debug("Player set changed - scheduling injection + direct write");
                        }
                    }
                    __finally
                    {
                        mtx.unlock();
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            static std::atomic<bool> s_crashLogged{false};
            if (!s_crashLogged.exchange(true, std::memory_order_relaxed))
                DMK::log().warning("Resolve: SEH caught crash");
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
        if (!DMK::memory::is_plausible_ptr(DMK::Address{a1}))
            return false;

        auto &ps = player_state();

        if (flag_fallback_mode().load(std::memory_order_relaxed))
        {
            // Actor type byte: *(*(actor+0x88)+1). Value 1 is the local player and 3 to 6 are party members. It is
            // the same mechanism the headgear visibility system uses.
            //
            // The walk is vis_ctrl +0x88 -> +0x08 -> +0x88 and the type byte sits at +1, all under fault guards. The
            // walk stops at the terminal +0x88 SLOT and the trailing read dereferences it, so typePtr carries
            // *(actor+0x88). This fallback runs when the global chain-walk AOB failed at init, so the downstream
            // links are not proven live.
            const auto typePtr =
                DMK::memory::walk(DMK::Address{a1}, std::array<std::ptrdiff_t, 3>{0x88, 0x08, 0x88})
                    .and_then([](DMK::Address leaf) { return DMK::memory::read<std::uintptr_t>(leaf); });
            if (typePtr)
            {
                const auto typeByteOpt = DMK::memory::read<std::uint8_t>(DMK::Address{*typePtr + 1});
                if (typeByteOpt)
                {
                    const std::uint8_t typeByte = *typeByteOpt;
                    {
                        static std::atomic<int> s_fbLog{0};
                        if (s_fbLog.fetch_add(1, std::memory_order_relaxed) < 5)
                            DMK::log()
                                .trace("Fallback chain: a1=0x{:X} typePtr=0x{:X} type={}", a1, *typePtr, typeByte);
                    }
                    bool isProtagonist = (typeByte == 1) || (typeByte >= 3 && typeByte <= 6);
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
                            if (ps.count.compare_exchange_weak(expected, n + 1, std::memory_order_relaxed))
                            {
                                ps.visCtrls[n].store(a1, std::memory_order_relaxed);
                                // The fallback path runs when the global chain-walk AOB failed at init, so the body
                                // pointer behind `a1` is not directly reachable. Mark the slot's identity unknown.
                                // Consumers then use the active character's hide mask, which keeps single-character
                                // semantics for an unidentified slot.
                                ps.visCharIdx[n].store(-1, std::memory_order_relaxed);
                                ps.primaryVisCtrl.store(
                                    ps.visCtrls[0].load(std::memory_order_relaxed),
                                    std::memory_order_relaxed
                                );
                                needs_direct_write().store(true, std::memory_order_release);
                                DMK::log().debug(
                                    "Fallback: cached protagonist vis ctrl at slot {} (0x{:X}, type={})",
                                    n,
                                    a1,
                                    typeByte
                                );
                            }
                        }
                    }
                }
            }

            // Fail closed. Until the fallback path caches at least one protagonist, do NOT admit the candidate. An
            // admission during the resolve gap leaks hides onto NPCs that no runtime pass can restore, because their
            // vis ctrls never entered the active set and the orphan sweep skips them.
            return ps.count.load(std::memory_order_relaxed) > 0 && is_player_vis_ctrl(a1);
        }

        // Cold start only. Resolve inline when no protagonist is published yet, never on a periodic refresh.
        //
        // This body runs on a game thread and resolve_player_vis_ctrls() is not cheap. Its actor sweep runs the
        // appearance-config classifier over the manager's actor array and early-outs only once it finds BOTH
        // companions, so solo play, or any moment a companion is not spawned, walks the array end to end. The bound
        // is the engine's array capacity, which ratchets up with each zone load and never shrinks. Cost per call
        // therefore grows with the number of transitions in a session while the call rate stays flat, and a periodic
        // refresh here becomes a per-frame stall that only surfaces deep into a session.
        //
        // The resolve-poll thread covers the steady state instead. It re-resolves once a second off the game thread
        // and reacts to actor rotation directly, so a refusal to refresh from the hook loses nothing. Only the cold
        // path stays, because until the count is non-zero this filter rejects every part and the mod is inert.
        if (ps.count.load(std::memory_order_relaxed) == 0)
        {
            const auto now = steady_ms();
            auto last = s_lastInlineResolveMs.load(std::memory_order_relaxed);
            if ((now - last) >= k_inlineResolveMinIntervalMs &&
                s_lastInlineResolveMs
                    .compare_exchange_strong(last, now, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                resolve_player_vis_ctrls();
            }
        }

        // Fail closed (see the fallback branch above for the rationale).
        return ps.count.load(std::memory_order_relaxed) > 0 && is_player_vis_ctrl(a1);
    }

} // namespace EquipHide
