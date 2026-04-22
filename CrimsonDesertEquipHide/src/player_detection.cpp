#include "player_detection.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    static std::atomic<uint32_t> s_resolveCounter{0};
    static constexpr uint32_t k_resolveInterval = 512;

    static uintptr_t s_prevVisCtrls[k_maxProtagonists]{};
    static int s_prevCount = 0;

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
            DMK::Logger::get_instance().trace(
                "body_to_vis_ctrl: body=0x{:X} inner=NULL (+0x68)", body);
            return 0;
        }
        auto sub = read_ptr_unsafe(inner, 0x40);
        if (!sub)
        {
            DMK::Logger::get_instance().trace(
                "body_to_vis_ctrl: body=0x{:X} inner=0x{:X} sub=NULL (+0x40)",
                body, inner);
            return 0;
        }
        auto vc = read_ptr_unsafe(sub, 0xE8);

        // Dedupe on (body, vc): only log when this body resolves to a
        // different vc than last seen. Small fixed LRU sized to the max
        // protagonist count -- any more is not useful for this resolver.
        struct Entry
        {
            uintptr_t body, vc;
        };
        static Entry s_lru[k_maxProtagonists]{};
        static int s_next = 0;
        for (auto &e : s_lru)
        {
            if (e.body == body && e.vc == vc)
                return vc;
        }
        s_lru[s_next] = {body, vc};
        s_next = (s_next + 1) % k_maxProtagonists;

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
                    CDCore::invalidate_controlled_character();
                }
                s_prevUser = user;
            }

            /* Character-swap invalidation. In-session protagonist
               swaps (Kliff <-> Damiane <-> Oongka) rotate user+0xD8 to
               point at a different ClientChildOnlyInGameActor without
               touching the UserActor singleton above, so the first
               trigger does not catch them. Without this second point
               the LKG identity cached by CDCore against the prior
               actor stays valid from Core's perspective and the
               resolver's torn-read absorption returns the previous
               character's name on any chain walk that lands on the
               new wrapper before its +0x60 slot-index byte is
               populated. Result downstream: set_active_character()
               gets called with the wrong idx for one or more ticks
               after the swap and the per-character Parts list
               switches to the outgoing character's overrides until
               the next tick re-classifies. Invalidating here costs at
               most one polling tick of additional latency (the
               resolver returns Unknown until the chain walk on the
               new wrapper lands a known identity key) which the
               caller tolerates by idx=-1 falling back to base Parts.

               The prevControlledActor != 0 guard mirrors the
               first-boot suppression above: on the very first tick
               the prior value is zero and the freshly-walked pointer
               is the initial controlled actor, so invalidating would
               wipe a cache just populated against it. */
            auto controlledActor = read_ptr_unsafe(user, 0xD8);
            static std::uintptr_t s_prevControlledActor = 0;
            if (controlledActor != s_prevControlledActor)
            {
                if (s_prevControlledActor != 0)
                {
                    CDCore::invalidate_controlled_character();
                }
                s_prevControlledActor = controlledActor;
            }

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

            static constexpr ptrdiff_t k_bodyOffsets[] = {
                0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100, 0x108};

            int count = 0;
            for (auto off : k_bodyOffsets)
            {
                if (count >= k_maxProtagonists)
                    break;

                /* Per-slot SEH so one bad body pointer does not abort the loop. */
                __try
                {
                    auto candidate = read_ptr_unsafe(user, off);
                    if (!candidate)
                        continue;

                    auto vt = read_ptr_unsafe(candidate, 0);
                    if (vt != addrs.childActorVtbl)
                        continue;

                    auto vc = body_to_vis_ctrl(candidate);
                    if (vc)
                        ps.visCtrls[count++] = vc;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }

            for (int i = count; i < k_maxProtagonists; ++i)
                ps.visCtrls[i].store(0, std::memory_order_relaxed);

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
        if (!flag_player_only().load(std::memory_order_relaxed))
            return true;

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

            return ps.count.load(std::memory_order_relaxed) <= 0 ||
                   is_player_vis_ctrl(a1);
        }

        // Global pointer chain mode.
        auto cnt = s_resolveCounter.fetch_add(1, std::memory_order_relaxed);
        if (ps.count.load(std::memory_order_relaxed) == 0 ||
            (cnt & (k_resolveInterval - 1)) == 0)
            resolve_player_vis_ctrls();

        return ps.count.load(std::memory_order_relaxed) <= 0 ||
               is_player_vis_ctrl(a1);
    }

} // namespace EquipHide
