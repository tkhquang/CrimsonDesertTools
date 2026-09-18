#include "bald_fix.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace EquipHide
{
    namespace
    {
        std::atomic<PostfixEvalFn> s_original_postfix_eval{nullptr};

        // A call-graph landmark decides player-vs-prefab-instantiation identity. Every PostfixEval invocation that
        // arrives through the engine's `createPrefabFromPartPrefab` path carries the post-call return address at
        // ResolvedAddresses::npc_pfe_return_addr on its stack. That path allocates the 240-byte prefab instance, then
        // invokes the rule pipeline. A player-side PostfixEval invocation runs from the equipment-visibility update
        // loop and never includes that address.
        //
        // At hook entry the detour scans a small stack window. When the landmark is present the detour defers to the
        // original evaluator and mutates no item bitmask. There is no ctx cache, no frequency threshold and no
        // per-item hash heuristic. The scan window must be deep enough to span every wrapper frame between
        // PostfixEval and the prefab-build call site. 64 slots cover that depth with margin.
        constexpr int STACK_SCAN_DEPTH = 64;

        // Dedup tables so the trace log fires at most once per ever-seen ctx. Sizing rationale: the game has at most
        // three player protagonists, and one load zone produces few NPC prefab-instantiation contexts. Tables never
        // evict. On saturation a further context silently stops logging. Entries are zero-initialized, with 0 as the
        // empty sentinel.
        constexpr int LOG_DEDUP_SIZE = 16;
        std::atomic<uintptr_t> s_logged_accept_ctxs[LOG_DEDUP_SIZE]{};
        std::atomic<uintptr_t> s_logged_reject_ctxs[LOG_DEDUP_SIZE]{};

        // Returns true exactly once per (table, ctx) pair, on the first call that inserts ctx into a free slot. Safe
        // under contention: compare_exchange distinguishes an insert from a lost race to the same ctx. Lock-free,
        // with no per-call allocation.
        bool log_dedup_claim(std::atomic<uintptr_t> *table, uintptr_t ctx) noexcept
        {
            for (int i = 0; i < LOG_DEDUP_SIZE; ++i)
            {
                const auto v = table[i].load(std::memory_order_relaxed);
                if (v == ctx)
                    return false;
                if (v == 0)
                {
                    uintptr_t expected = 0;
                    if (table[i].compare_exchange_strong(
                            expected,
                            ctx,
                            std::memory_order_relaxed,
                            std::memory_order_relaxed
                        ))
                        return true;
                    // Another thread claimed this slot. When it claimed the slot with the same ctx the work is done.
                    // Otherwise keep scanning.
                    if (expected == ctx)
                        return false;
                }
            }
            return false;
        }

        // Priority bitmask layout (item+0x70, DWORD):
        //   bits 16-22: "priority level exists"
        //   bits  0-6:  "priority level active"
        //
        // Bit 19 = priority 3 exists. Without bit 3 (active), PostfixEval sees priority 3 as highest but inactive, so
        // the rule does not match and hair stays visible. Official HideAlways does the same: 0x00010001 -> 0x00090001.

        constexpr uint32_t PRIORITY_BIT = 0x00080000u; // bit 19
        constexpr uint32_t ACTIVE_BIT = 0x00000008u;   // bit 3
        constexpr int MAX_ITEMS = 128;

        bool is_hair_hiding_rule(__int64 rule_obj) noexcept
        {
            const auto handle_addr = *reinterpret_cast<const uintptr_t *>(rule_obj + 0x18);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{handle_addr}))
                return false;
            const auto suffix_addr = *reinterpret_cast<const uintptr_t *>(handle_addr);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{suffix_addr}))
                return false;

            const auto *suffix = reinterpret_cast<const char *>(suffix_addr);
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

        // Build a bitmask of hidden head-covering categories for per-item filtering. Only an item that belongs to a
        // hidden category gets the priority override. An item for a shown category keeps its original bitmask, so
        // PostfixEval still hides hair and beard under it.
        CategoryMask hidden_headgear_mask() noexcept
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

        // Returns true when the current call stack came through the NPC-side PostfixEval caller. Scans a small window
        // of stack slots for resolved_addrs().npc_pfe_return_addr as a return address. The landmark always sits at a
        // predictable depth within the captured window, so a bounded scan is sufficient and cheap. SEH wraps the walk
        // in case an unusually deep call trimmed the frame.
        bool is_npc_call_stack(uintptr_t landmark) noexcept
        {
            if (!landmark)
                return false;

            auto *frame = static_cast<uintptr_t *>(_AddressOfReturnAddress());
            __try
            {
                for (int i = 0; i < STACK_SCAN_DEPTH; ++i)
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

        // Temporarily set bit 19 on an item whose category is hidden, call the original PostfixEval, then restore.
        // Only an item that belongs to a hidden headgear category is overridden. A hidden Mask therefore overrides
        // only Mask items and the beard stays visible, while Helm items keep their bitmask and hair hides under the
        // helmet as normal.
        //
        // Every foreign access below goes through a guarded memory:: call, so a context that slips through the
        // validator cannot crash the game and the function needs no SEH frame of its own. A failed patch write drops
        // the item from the restore list, so the restore loop never writes an item this pass did not change.
        __int64 eval_with_priority_override(__int64 rule_obj, __int64 context) noexcept
        {
            const DMK::Address ctx{static_cast<uintptr_t>(context)};
            const auto items_ptr = DMK::memory::read<std::uintptr_t>(ctx.offset(0x58));
            const auto itemCount = DMK::memory::read<std::uint32_t>(ctx.offset(0x60));

            if (!items_ptr || !itemCount || !DMK::memory::is_plausible_ptr(DMK::Address{*items_ptr}) ||
                *itemCount == 0 || *itemCount > static_cast<std::uint32_t>(MAX_ITEMS))
            {
                // Snapshot guards a teardown race: shutdown's remove_hook() disables the detour while a game thread
                // can still be mid-call here. A null trampoline means the hook already restored the prologue and tore
                // the trampoline down. A return of 0 matches the rule evaluator's "no rule fired" shape.
                const auto trampoline = s_original_postfix_eval.load(std::memory_order_relaxed);
                if (!trampoline)
                    return 0;
                return trampoline(rule_obj, context);
            }

            const auto hidden_mask = hidden_headgear_mask();

            std::array<std::uintptr_t, MAX_ITEMS> patched_items{};
            std::array<std::uint32_t, MAX_ITEMS> original_values{};
            int patch_count = 0;

            // The equipped-item container at context+0x58 is not a flat array of item pointers. Each entry is 0x10
            // bytes with a dword part hash at entry+0x00 and the ITEM POINTER at entry+0x08. The engine's own loop
            // header uses the same shape (`mov edi,[rdx+0x60] ; shl rdi,4 ; add rdi,rbx`, a 0x10 stride), and many
            // independent engine sites walk this same [ctx+0x58]/[ctx+0x60] container with stride 0x10 and
            // dereference +0x08.
            //
            // The entry stride and the pointer offset move when the engine reshapes this container. A stale stride
            // fails SILENTLY: a stride-8 walk reads a hash dword as an item pointer on every other index, misses the
            // plausibility gate below, and skips half the wardrobe with no error. A correct AOB alone does not
            // restore the bald fix. Verify both constants against live memory on patch day.
            constexpr uintptr_t item_entry_stride = 0x10;
            constexpr uintptr_t item_entry_ptr_offset = 0x08;

            for (uint32_t i = 0; i < *itemCount; ++i)
            {
                const auto entry_offset = static_cast<std::ptrdiff_t>(i * item_entry_stride + item_entry_ptr_offset);
                const auto item = DMK::memory::read<std::uintptr_t>(DMK::Address{*items_ptr}.offset(entry_offset));
                if (!item || !DMK::memory::is_plausible_ptr(DMK::Address{*item}))
                    continue;

                const DMK::Address item_base{*item};
                const auto equipped = DMK::memory::read<std::uint8_t>(item_base.offset(0x88));
                if (!equipped || *equipped == 0)
                    continue;

                // Only override an item whose part hash belongs to a hidden head-covering category.
                const auto part_hash = DMK::memory::read<std::uint32_t>(item_base.offset(0x48));
                if (!part_hash || !needs_classification(*part_hash))
                    continue;
                const auto part_cat = classify_part(*part_hash);
                if ((part_cat & hidden_mask) == 0)
                    continue;

                const auto bitmask = DMK::memory::read<std::uint32_t>(item_base.offset(0x70));
                if (!bitmask || (*bitmask & PRIORITY_BIT))
                    continue;

                const std::uint32_t patched = (*bitmask | PRIORITY_BIT) & ~ACTIVE_BIT;
                if (!DMK::memory::write_in_place<std::uint32_t>(item_base.offset(0x70), patched))
                    continue;
                patched_items[static_cast<std::size_t>(patch_count)] = *item;
                original_values[static_cast<std::size_t>(patch_count)] = *bitmask;
                ++patch_count;
            }

            // Snapshot guards a teardown race (see the early-out branch above). This point is past patch_count item
            // mutations, so the restore loop must run either way. A null trampoline yields a zero result and skips no
            // cleanup.
            const auto trampoline = s_original_postfix_eval.load(std::memory_order_relaxed);
            const __int64 result = trampoline ? trampoline(rule_obj, context) : 0;

            for (int i = 0; i < patch_count; ++i)
            {
                const auto slot = static_cast<std::size_t>(i);
                (void)DMK::memory::write_in_place<std::uint32_t>(
                    DMK::Address{patched_items[slot]}.offset(0x70),
                    original_values[slot]
                );
            }

            return result;
        }
    } // namespace

    void set_postfix_eval_trampoline(PostfixEvalFn original)
    {
        s_original_postfix_eval.store(original, std::memory_order_relaxed);
    }

    __int64 __fastcall on_postfix_eval(__int64 rule_obj, __int64 context)
    {
        if (flag_bald_fix().load(std::memory_order_relaxed))
        {
            __try
            {
                const auto ctx = static_cast<uintptr_t>(context);

                // Consider an override only for a hair-hiding rule with some head-covering gear hidden. These gates
                // are cheap and ordered first, so most calls exit without a touch of the stack-walk filter.
                if (DMK::memory::is_plausible_ptr(DMK::Address{ctx}) && is_hair_hiding_rule(rule_obj) &&
                    (is_category_hidden(Category::Helm) || is_category_hidden(Category::Cloak) ||
                     is_category_hidden(Category::Mask)))
                {
                    auto &logger = DMK::log();
                    if (is_npc_call_stack(resolved_addrs().npc_pfe_return_addr))
                    {
                        if (log_dedup_claim(s_logged_reject_ctxs, ctx))
                            logger.trace("BaldFix: filtered prefab-instantiation call (ctx=0x{:X})", ctx);
                        // Fall through to the original evaluator untouched.
                    }
                    else
                    {
                        if (log_dedup_claim(s_logged_accept_ctxs, ctx))
                            logger.trace("BaldFix: override active (ctx=0x{:X})", ctx);
                        return eval_with_priority_override(rule_obj, context);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        // Snapshot guards a teardown race (see eval_with_priority_override).
        const auto trampoline = s_original_postfix_eval.load(std::memory_order_relaxed);
        if (!trampoline)
            return 0;
        return trampoline(rule_obj, context);
    }

} // namespace EquipHide
