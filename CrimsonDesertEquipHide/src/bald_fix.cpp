#include "bald_fix.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <cstdint>

namespace EquipHide
{
    static PostfixEvalFn s_originalPostfixEval = nullptr;

    // A call-graph landmark decides player-vs-prefab-instantiation identity. Every PostfixEval invocation that comes
    // through the engine's `createPrefabFromPartPrefab` path carries the post-call return address at
    // ResolvedAddresses::npcPfeReturnAddr on its stack. That path allocates the 240-byte prefab instance, then invokes
    // the rule pipeline. Player-side PostfixEval invocations run from the equipment-visibility update loop and never
    // include that address. At hook entry we scan a small stack window. If the landmark is present, we defer to the
    // original evaluator without mutating any item bitmasks. No ctx caching, no frequency threshold, no per-item hash
    // heuristic. The scan window must be deep enough to span every wrapper frame between PostfixEval and the
    // prefab-build call site. 64 slots cover that depth with margin.
    static constexpr int k_stackScanDepth = 64;

    // Dedup tables so the trace log fires at most once per ever-seen ctx. Sizing rationale: the game has at most three
    // player protagonists, and one load zone produces few NPC prefab-instantiation contexts. Tables never evict. If
    // they saturate, further contexts silently stop logging. Entries are zero-initialized (0 sentinel = empty).
    static constexpr int k_logDedupSize = 16;
    static std::atomic<uintptr_t> s_loggedAcceptCtxs[k_logDedupSize]{};
    static std::atomic<uintptr_t> s_loggedRejectCtxs[k_logDedupSize]{};

    /* Returns true exactly once per (table, ctx) pair -- on the first call that inserts ctx into a free slot. Safe
       under contention: compare_exchange distinguishes "we inserted" from "lost the race to the same ctx". Lock-free,
       no per-call allocation. */
    static bool log_dedup_claim(std::atomic<uintptr_t> *table, uintptr_t ctx) noexcept
    {
        for (int i = 0; i < k_logDedupSize; ++i)
        {
            auto v = table[i].load(std::memory_order_relaxed);
            if (v == ctx)
                return false;
            if (v == 0)
            {
                uintptr_t expected = 0;
                if (table[i].compare_exchange_strong(expected, ctx, std::memory_order_relaxed,
                                                     std::memory_order_relaxed))
                    return true;
                /* Race: another thread claimed this slot. If it claimed the slot with the same ctx, we are done.
                   Otherwise keep scanning. */
                if (expected == ctx)
                    return false;
            }
        }
        return false;
    }

    void set_postfix_eval_trampoline(PostfixEvalFn original)
    {
        s_originalPostfixEval = original;
    }

    // Priority bitmask layout (item+0x70, DWORD):
    //   bits 16-22: "priority level exists"
    //   bits  0-6:  "priority level active"
    //
    // Bit 19 = priority 3 exists. Without bit 3 (active), PostfixEval sees priority 3 as highest but inactive, so the
    // rule does not match and hair stays visible. Official HideAlways does the same: 0x00010001 -> 0x00090001.

    static constexpr uint32_t k_priorityBit = 0x00080000u; // bit 19
    static constexpr uint32_t k_activeBit = 0x00000008u;   // bit 3
    static constexpr int k_maxItems = 128;

    static bool is_hair_hiding_rule(__int64 ruleObj) noexcept
    {
        auto handleAddr = *reinterpret_cast<const uintptr_t *>(ruleObj + 0x18);
        if (handleAddr < 0x10000)
            return false;
        auto suffixAddr = *reinterpret_cast<const uintptr_t *>(handleAddr);
        if (suffixAddr < 0x10000)
            return false;

        const auto *suffix = reinterpret_cast<const char *>(suffixAddr);
        if (suffix[0] != '_' || suffix[2] != '\0')
            return false;

        switch (suffix[1])
        {
        case 'a':
        case 'c':
        case 'd':
        case 'f':
        case 'i':
        case 'q':
        case 'v':
            return true;
        default:
            return false;
        }
    }

    /* Build a bitmask of hidden head-covering categories for per-item filtering. Only items belonging to a hidden
       category get the priority override -- items for shown categories keep their original bitmask so PostfixEval still
       hides hair/beard under them. */
    static CategoryMask hidden_headgear_mask() noexcept
    {
        CategoryMask mask = 0;
        if (is_category_hidden(Category::Helm))
            mask |= category_bit(Category::Helm);
        if (is_category_hidden(Category::Cloak))
            mask |= category_bit(Category::Cloak);
        if (is_category_hidden(Category::Mask))
            mask |= category_bit(Category::Mask);
        return mask;
    }

    /* Returns true if the current call stack came through the NPC-side PostfixEval caller. Scans a small window of
       stack slots for resolved_addrs().npcPfeReturnAddr as a return address. The landmark always sits at a predictable
       depth within the captured window, so bounded scanning is sufficient and cheap. SEH wraps the walk in case an
       unusually deep call trimmed the frame. */
    static bool is_npc_call_stack(uintptr_t landmark) noexcept
    {
        if (!landmark)
            return false;

        auto *frame = static_cast<uintptr_t *>(_AddressOfReturnAddress());
        __try
        {
            for (int i = 0; i < k_stackScanDepth; ++i)
            {
                if (frame[i] == landmark)
                    return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return false;
    }

    /* Temporarily set bit 19 on items whose category is hidden, call the original PostfixEval, then restore. Only
       items that belong to a hidden headgear category are overridden -- e.g. hiding Mask only overrides Mask items
       (beard stays visible) while Helm items keep their bitmask (hair hidden under helmet as normal).

       Per-item SEH guards against mid-walk faults: a context that slips through the validator must not be able to
       crash the game via a bad deref. Restoration is also SEH-wrapped so a fault on item[N] does not skip restoring
       item[N+1..]. */
    static __int64 eval_with_priority_override(__int64 ruleObj, __int64 context) noexcept
    {
        auto itemsPtr = *reinterpret_cast<uintptr_t *>(context + 0x58);
        auto itemCount = *reinterpret_cast<uint32_t *>(context + 0x60);

        if (itemsPtr < 0x10000 || itemCount == 0 || itemCount > k_maxItems)
        {
            // Snapshot guards a teardown race: shutdown's remove_hook() disables the detour while a game thread can
            // still be mid-call here. NULL means SafetyHook already restored the prologue and tore the trampoline
            // down. Returning 0 matches the rule-evaluator's "no rule fired" shape.
            auto trampoline = s_originalPostfixEval;
            if (!trampoline)
                return 0;
            return trampoline(ruleObj, context);
        }

        auto hiddenMask = hidden_headgear_mask();

        uintptr_t patchedItems[k_maxItems];
        uint32_t originalValues[k_maxItems];
        int patchCount = 0;

        // The equipped-item container at context+0x58 is not a flat array of item pointers. Each entry is 0x10 bytes
        // with a dword part hash at entry+0x00 and the ITEM POINTER at entry+0x08. The engine's own loop header uses
        // the same shape (`mov edi,[rdx+0x60] ; shl rdi,4 ; add rdi,rbx` -- a 0x10 stride), and many independent
        // engine sites walk this same [ctx+0x58]/[ctx+0x60] container with stride 0x10 and dereference +0x08.
        //
        // The entry stride and the pointer offset move when the engine reshapes this container. A stale stride fails
        // SILENTLY: a stride-8 walk reads a hash dword as an item pointer on every other index, faults into the
        // per-item SEH below, and skips half the wardrobe with no error. A correct AOB alone does not restore the
        // bald fix. Verify both constants against live memory on patch day.
        static constexpr uintptr_t k_itemEntryStride = 0x10;
        static constexpr uintptr_t k_itemEntryPtrOffset = 0x08;

        for (uint32_t i = 0; i < itemCount; ++i)
        {
            __try
            {
                auto item = *reinterpret_cast<uintptr_t *>(itemsPtr + i * k_itemEntryStride + k_itemEntryPtrOffset);
                if (item < 0x10000)
                    continue;
                if (*reinterpret_cast<uint8_t *>(item + 0x88) == 0)
                    continue;

                /* Only override items whose part hash belongs to a hidden head-covering category. */
                auto partHash = *reinterpret_cast<uint32_t *>(item + 0x48);
                if (!needs_classification(partHash))
                    continue;
                auto partCat = classify_part(partHash);
                if ((partCat & hiddenMask) == 0)
                    continue;

                auto *bm = reinterpret_cast<uint32_t *>(item + 0x70);
                uint32_t val = *bm;

                if (val & k_priorityBit)
                    continue;

                *bm = (val | k_priorityBit) & ~k_activeBit;
                patchedItems[patchCount] = item;
                originalValues[patchCount] = val;
                ++patchCount;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        // Snapshot guards a teardown race (see the early-out branch above). If we got this far, we already mutated
        // patchCount item bytes and must run the restore loop regardless. A NULL trampoline returns a zero result
        // without skipping cleanup.
        auto trampoline = s_originalPostfixEval;
        __int64 result = trampoline ? trampoline(ruleObj, context) : 0;

        for (int i = 0; i < patchCount; ++i)
        {
            __try
            {
                *reinterpret_cast<uint32_t *>(patchedItems[i] + 0x70) = originalValues[i];
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        return result;
    }

    __int64 __fastcall on_postfix_eval(__int64 ruleObj, __int64 context)
    {
        if (flag_bald_fix().load(std::memory_order_relaxed))
        {
            __try
            {
                auto ctx = static_cast<uintptr_t>(context);

                /* Only consider overrides for hair-hiding rules + any head-covering gear hidden. These gates are cheap
                   and ordered first so most calls exit without touching the stack-walk filter. */
                if (ctx > 0x10000 && is_hair_hiding_rule(ruleObj) &&
                    (is_category_hidden(Category::Helm) || is_category_hidden(Category::Cloak) ||
                     is_category_hidden(Category::Mask)))
                {
                    auto &logger = DMK::Logger::get_instance();
                    if (is_npc_call_stack(resolved_addrs().npcPfeReturnAddr))
                    {
                        if (log_dedup_claim(s_loggedRejectCtxs, ctx))
                            logger.trace("BaldFix: filtered prefab-instantiation "
                                         "call (ctx=0x{:X})",
                                         ctx);
                        /* Fall through to original evaluator untouched. */
                    }
                    else
                    {
                        if (log_dedup_claim(s_loggedAcceptCtxs, ctx))
                            logger.trace("BaldFix: override active (ctx=0x{:X})", ctx);
                        return eval_with_priority_override(ruleObj, context);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        // Snapshot guards a teardown race (see eval_with_priority_override).
        auto trampoline = s_originalPostfixEval;
        if (!trampoline)
            return 0;
        return trampoline(ruleObj, context);
    }

} // namespace EquipHide
