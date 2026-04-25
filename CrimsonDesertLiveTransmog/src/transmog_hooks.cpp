#include "transmog_hooks.hpp"
#include "shared_state.hpp"
#include "transmog_worker.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace Transmog
{
    static VisualEquipChangeFn s_origVEC = nullptr;
    static BatchEquipFn s_origBatchEquip = nullptr;

    VisualEquipChangeFn &orig_vec() { return s_origVEC; }
    BatchEquipFn &orig_batch_equip() { return s_origBatchEquip; }

    bool is_player_actor(__int64 a1) noexcept
    {
        // Originally this checked `*(typeEntry+1) == 1` (role byte).
        // CE probing on 1.03.01 showed the byte is character-specific
        // (Kliff=1, Damiane=4, Oongka=?), not a clean player/NPC flag,
        // so the equality check silently rejected every companion.
        //
        // New filter: pointer chain is readable AND typeEntry+8 is a
        // non-null pointer. For every party member we sampled, this
        // value is the same ActorManager pointer; for unrelated NPCs
        // the pointer differs or the chain faults. Good enough for
        // the transmog hooks -- misclassification just produces a
        // harmless no-op apply (no presets exist for unknown chars).
        //
        // DMK::Memory::read_ptr_unsafe is SEH-protected on MSVC and
        // VirtualQuery-guarded on MinGW; on fault it returns 0, which
        // the < 0x10000 guard rejects. This avoids the SEH-vs-C++-
        // destructors restriction that previously forced this whole
        // chain into a single __try frame.
        if (a1 < 0x10000)
            return false;

        const auto actor = static_cast<__int64>(
            DMK::Memory::read_ptr_unsafe(static_cast<uintptr_t>(a1), 8));
        if (actor < 0x10000)
            return false;
        const auto typeEntry = DMK::Memory::read_ptr_unsafe(
            static_cast<uintptr_t>(actor), 0x88);
        if (typeEntry < 0x10000)
            return false;
        const auto actorMgr = DMK::Memory::read_ptr_unsafe(typeEntry, 8);
        return actorMgr >= 0x10000;
    }

    // Slot tags we transmog. Everything outside this set (weapons, rings,
    // shoulders, belts, etc.) must not trigger apply_all_transmog.
    // CE-verified via hardware BP on sub_148EB6700.
    static constexpr bool is_armor_slot_tag(__int16 slotId) noexcept
    {
        const auto t = static_cast<std::uint16_t>(slotId);
        return t == 0x0003 || t == 0x0004 || t == 0x0005 ||
               t == 0x0006 || t == 0x0010;
    }

    // --- VEC hook (sub_14076D520) ---

    // Returns true iff a1 matches the controlled-character's actor
    // component (WS → ActorManager+0x30 → UserActor+0x28 → +0xD8).
    // Early-outs on mismatch prevent LT from re-running its carrier-
    // apply on every background companion's equip/VEC event -- each
    // such unintended apply was mutating scene-graph state for the
    // wrong actor, accumulating dangling pointers and crashing the
    // game on later traversals (weapon draw, glide, etc.).
    static bool is_controlled_actor(__int64 a1) noexcept
    {
        if (a1 < 0x10000)
            return false;
        const auto controlled = resolve_player_component();
        if (!controlled)
            return true; // can't resolve → don't filter
        return a1 == controlled;
    }

    __int64 __fastcall on_vec(__int64 a1, __int16 slotId, __int16 itemId, __int64 a4)
    {
        // Snapshot the trampoline pointer at entry. Even with SafetyHook
        // draining in-flight callers under its shared lock during
        // shutdown, a game thread that re-enters the patched site
        // between the drain and the DLL unmap can observe a torn
        // trampoline. A null snapshot means teardown is in progress;
        // bail with a benign zero return rather than dereferencing
        // through a dangling pointer.
        const auto trampoline = s_origVEC;
        if (!trampoline)
            return 0;

        // Recursion guard + suppress during clear+apply cycle.
        if (in_transmog().load(std::memory_order_relaxed) ||
            suppress_vec().load(std::memory_order_acquire))
            return trampoline(a1, slotId, itemId, a4);

        __int64 ret = trampoline(a1, slotId, itemId, a4);

        if (!flag_enabled().load(std::memory_order_relaxed))
            return ret;

        if (flag_player_only().load(std::memory_order_relaxed) && !is_player_actor(a1))
            return ret;

        // Only schedule apply for the currently-controlled character.
        // Background companion equip events route through VEC with
        // their own a1 and must not drive LT's dispatch.
        if (!is_controlled_actor(a1))
            return ret;

        // Skip non-armor slot changes (weapons, rings, etc.). We still
        // capture a1 so manual applies have the latest player pointer.
        player_a1().store(a1, std::memory_order_release);
        if (!is_armor_slot_tag(slotId))
            return ret;

        schedule_transmog(a1, 0);

        return ret;
    }

    // --- BatchEquip hook (sub_14075BBF0) ---

    static uint32_t *__fastcall on_batch_equip_impl(
        __int64 a1, uint32_t *a2, __int64 **a3, __int64 **a4)
    {
        // Snapshot the trampoline pointer at entry; see on_vec for the
        // teardown-window rationale. Returning the caller's a2 buffer
        // pointer matches the upstream contract for a no-op pass-through
        // (the game checks the pointer for non-null, not the buffer
        // contents, when an early bail is needed).
        const auto trampoline = s_origBatchEquip;
        if (!trampoline)
            return a2;

        if (in_transmog().load(std::memory_order_relaxed))
            return trampoline(a1, a2, a3, a4);

        uint32_t *ret = trampoline(a1, a2, a3, a4);

        if (flag_enabled().load(std::memory_order_relaxed) &&
            (!flag_player_only().load(std::memory_order_relaxed) || is_player_actor(a1)) &&
            is_controlled_actor(a1))
        {
            // A save-load or zone transition routes a different a1
            // through this path. Every cached "damaged" flag and
            // lastAppliedRealIds entry belongs to the previous
            // incarnation and must be wiped before the next apply.
            const __int64 prevA1 =
                player_a1().exchange(a1, std::memory_order_acq_rel);
            if (prevA1 != a1)
            {
                real_damaged().fill(false);
                last_applied_real_ids().fill(0);
            }

            DMK::Logger::get_instance().info(
                "BatchEquip[player]: a1={:#018x}, *(a1+8)={:#018x}, scheduling transmog",
                static_cast<uint64_t>(a1),
                a1 > 0x10000 ? static_cast<uint64_t>(*reinterpret_cast<uintptr_t *>(a1 + 8)) : 0ULL);
            schedule_transmog(a1, 0);
        }

        return ret;
    }

    uint32_t *__fastcall on_batch_equip(
        __int64 a1, uint32_t *a2, __int64 **a3, __int64 **a4)
    {
        __try
        {
            return on_batch_equip_impl(a1, a2, a3, a4);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Mirror the impl's null-snapshot guard: if SEH fires while
            // the trampoline has just been swapped out, do not deref
            // through a dangling pointer.
            const auto trampoline = s_origBatchEquip;
            return trampoline ? trampoline(a1, a2, a3, a4) : a2;
        }
    }

} // namespace Transmog
