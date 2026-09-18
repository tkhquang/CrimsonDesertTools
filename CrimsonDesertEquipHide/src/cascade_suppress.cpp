#include "cascade_suppress.hpp"
#include "aob_resolver.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <DetourModKit/logger.hpp>
#include <DetourModKit/scan.hpp>

#include <Windows.h>

#include <cstddef>
#include <span>

namespace EquipHide
{
    static std::atomic<bool> s_equipChangeDetected{false};

    // Slot id for chest armor. Only a chest change needs a cascade re-sync -- the other armor slots (necklace=9,
    // mask=18, and so on) do not affect the chest lock state.
    static constexpr uint16_t k_chestSlot = 4;

    // BatchEquip dispatch-entry layout, used to read the per-entry slot id.
    //
    // The engine walks two nested containers here, and the mod must read from the outer one. The outer loop iterates
    // the swap entries handed to on_visual_equip_swap: it loads the list head, takes the count from the head's `+8`,
    // and forms the end pointer with an `imul <reg>, rax, <stride>`. The inner loop searches one actor's authority
    // table with a different stride and a different slot field. A read of the inner values mis-indexes every entry,
    // so the mod decodes both constants below from the outer loop.
    //
    // The mod decodes both values live and does not hardcode them. The entry width changes across builds, so a fixed
    // size mis-reads the slot id silently instead of failing. The nominal is the last verified layout, and the code
    // uses it only when the decode fails. Verify it against live memory on patch day.
    //
    // Stride: the imm32 of the outer `imul <reg>, rax, <stride>`. No candidate pins that imm32. P1 and P2 stop
    // before it, and P3 wildcards it. The pattern therefore stays value-agnostic and self-heals when the entry width
    // changes.
    //
    // A short branch sits between the outer loop head and the imul, and the same three-instruction shape appears at an
    // unrelated container-iteration site. No branch-free window tells the two sites apart, so each candidate below
    // includes one short jump with a wildcarded target. If a future build changes that jump encoding, the candidate
    // stops matching. That result is intended: the decode falls back to the nominal instead of resolving to the wrong
    // site.
    //
    // Every walk-back below is the byte offset of the IMUL ITSELF within the matched window, which is what
    // read_code_constant decodes the operand from. It has to be re-measured whenever the instructions ahead of the
    // imul change length -- folding two loads into one is enough to shift it. A stale value decodes a neighboring
    // instruction's bytes as the stride, and the range check is the only thing standing between that and a silently
    // wrong entry width.
    const DMK::scan::Candidate k_equipSwapStrideSite[] = {
        // P1 -- outer loop head through the imul. Widest context, anchored before the null check.
        DMK::scan::Candidate::direct(
            "BatchEquipStride_P1_LoopHeadToImul",
            DMK::scan::Pattern::literal("C7 85 ?? ?? ?? ?? ?? ?? ?? ?? 49 8B 19 48 85 DB 74 ?? 8B 43 08 48 69 F8"),
            0x15
        ),

        // P2 -- null check through the imul. Drops the preceding frame initialization.
        DMK::scan::Candidate::direct(
            "BatchEquipStride_P2_NullCheckToImul",
            DMK::scan::Pattern::literal("49 8B 19 48 85 DB 74 ?? 8B 43 08 48 69 F8"),
            11
        ),

        // P3 -- imul forward into the branch that follows it. Independent of everything before the imul, so it
        // survives a rewrite of the loop head that defeats P1 and P2.
        DMK::scan::Candidate::direct(
            "BatchEquipStride_P3_ImulToJoin",
            DMK::scan::Pattern::literal("8B 43 08 48 69 F8 ?? ?? ?? ?? 48 03 3B 48 8B 1B EB ?? 49 8B 49 08"),
            3
        ),
    };

    // Slot: the disp32 of `movzx eax, word ptr [rbx+<slot>]`, where rbx is the current outer entry. The instruction
    // that follows compares that word against the inner table's own slot field, which uses a different offset. Anchor
    // on the movzx, not on the compare.
    const DMK::scan::Candidate k_equipSwapSlotSite[] = {
        // P1 -- inner-loop setup through the movzx. Widest context and unique across the whole process.
        DMK::scan::Candidate::direct(
            "BatchEquipSlot_P1_InnerSetupToMovzx",
            DMK::scan::Pattern::literal("48 69 C8 ?? ?? ?? ?? 48 03 CA 48 3B D1 74 ?? 0F B7 83"),
            0x0F
        ),

        // P2 -- movzx and compare, extended into the entry advance that follows.
        DMK::scan::Candidate::direct(
            "BatchEquipSlot_P2_MovzxCompareAdvance",
            DMK::scan::Pattern::literal("0F B7 83 ?? ?? ?? ?? 66 39 82 ?? ?? ?? ?? 74 ?? 48 81 C2")
        ),

        // P3 -- movzx and compare only. Both displacements are wildcarded, so a shifted slot field still matches.
        DMK::scan::Candidate::direct(
            "BatchEquipSlot_P3_MovzxCompare",
            DMK::scan::Pattern::literal("0F B7 83 ?? ?? ?? ?? 66 39 82 ?? ?? ?? ??")
        ),
    };

    // Decode an instruction operand to a layout constant, validated to a plausible range. On any miss, out-of-range
    // value, or decode exception the code keeps the nominal. A wrong anchor or operand index therefore can never
    // mis-read the dispatch entry. The code logs the decoded value once for verification.
    [[nodiscard]] static std::size_t decode_layout_constant(
        std::span<const DMK::scan::Candidate> site,
        DMK::scan::OperandKind kind,
        std::uint8_t operandIndex,
        std::int64_t lo,
        std::int64_t hi,
        std::size_t nominal,
        const char *label
    ) noexcept
    {
        try
        {
            DMK::scan::CodeConstant cc{};
            cc.site = site;
            cc.kind = kind;
            cc.operand_index = operandIndex;
            cc.nominal = static_cast<std::int64_t>(nominal);
            cc.has_nominal = true;
            const auto decoded = DMK::scan::read_code_constant(cc);
            if (decoded.has_value() && *decoded >= lo && *decoded <= hi)
            {
                const auto value = static_cast<std::size_t>(*decoded);
                // A live value != nominal means the engine layout drifted on a patch. The decode self-heals it, but
                // reports a WARNING so the offset change is easy to find in the log.
                if (value != nominal)
                    DMK::log().warning(
                        "BatchEquip {} DRIFTED: live={} nominal={} -- self-healed (engine layout changed)",
                        label,
                        value,
                        nominal
                    );
                else
                    DMK::log().info("BatchEquip {} decoded live: {} (matches nominal)", label, value);
                return value;
            }
            DMK::log().warning("BatchEquip {} live-decode out of range/unavailable; using nominal {}", label, nominal);
        }
        catch (...)
        {
        }
        return nominal;
    }

    [[nodiscard]] static std::size_t equip_swap_entry_stride() noexcept
    {
        // Nominal is the outer entry stride, 240 (0xF0). The accepted range leaves headroom on both sides so a further
        // change of entry width still decodes instead of falling back.
        static const std::size_t value = decode_layout_constant(
            k_equipSwapStrideSite,
            DMK::scan::OperandKind::Immediate,
            2,
            216,
            264,
            240,
            "stride"
        );
        return value;
    }

    [[nodiscard]] static std::size_t equip_swap_slot_offset() noexcept
    {
        // Nominal is the slot field of the outer entry, 216 (0xD8). Do not set this to the inner table's slot field:
        // that one sits at a different offset and belongs to a container with a different stride.
        static const std::size_t value = decode_layout_constant(
            k_equipSwapSlotSite,
            DMK::scan::OperandKind::MemoryDisplacement,
            1,
            192,
            240,
            216,
            "slot"
        );
        return value;
    }

    // VisualEquipChange hook (equip/unequip)

    static VisualEquipChangeFn s_originalVisualEquipChange = nullptr;

    void set_visual_equip_change_trampoline(VisualEquipChangeFn original)
    {
        s_originalVisualEquipChange = original;
    }

    __int64 __fastcall on_visual_equip_change(__int64 bodyComp, int16_t slotId, int16_t itemId, __int64 itemData)
    {
        DMK::log().trace("VisualEquipChange: slot={} item={}", slotId, itemId);

        if (flag_cascade_fix().load(std::memory_order_relaxed) && is_category_hidden(Category::Chest) &&
            slotId == k_chestSlot)
        {
            DMK::log().debug("VisualEquipChange: chest slot={} item={} -- clearing cascade locks", slotId, itemId);
            s_equipChangeDetected.store(true, std::memory_order_relaxed);
        }
        // Snapshot guards a teardown race: dropping the Hook handle restores the prologue and disables the detour,
        // but a game thread already past the JMP can still enter the body before the DLL unmaps. A return of zero
        // matches the engine no-op shape for this slot-update API.
        auto trampoline = s_originalVisualEquipChange;
        if (!trampoline)
            return 0;
        return trampoline(bodyComp, slotId, itemId, itemData);
    }

    // VisualEquipSwap hook (direct item-to-item swap)

    static VisualEquipSwapFn s_originalVisualEquipSwap = nullptr;

    void set_visual_equip_swap_trampoline(VisualEquipSwapFn original)
    {
        s_originalVisualEquipSwap = original;
        // Warm the layout self-heal at install (setup/control-plane). The dispatch-entry stride and slot then decode
        // and cache before the first swap, and the hot path stays free of the one-time AOB scan.
        (void)equip_swap_entry_stride();
        (void)equip_swap_slot_offset();
    }

    __int64 __fastcall on_visual_equip_swap(__int64 *a1, __int64 *a2, __int64 **a3, __int64 **a4)
    {
        __try
        {
            // Log all swapped slots at trace level for future reference.
            auto &logger = DMK::log();
            __int64 *iter = a4 ? (*a4 ? *a4 : a4[1]) : nullptr;
            if (iter)
            {
                auto base = *iter;
                auto count = *reinterpret_cast<const uint32_t *>(reinterpret_cast<const char *>(iter) + 8);
                bool hasChest = false;
                for (uint32_t i = 0; i < count && i < 16; ++i)
                {
                    auto slot = *reinterpret_cast<const uint16_t *>(
                        base + equip_swap_entry_stride() * i + equip_swap_slot_offset()
                    );
                    logger.trace("EquipSwap: slot={}", slot);
                    if (slot == k_chestSlot)
                        hasChest = true;
                }

                if (hasChest && flag_cascade_fix().load(std::memory_order_relaxed) &&
                    is_category_hidden(Category::Chest))
                {
                    logger.debug("EquipSwap: chest slot detected -- signalling re-sync");
                    s_equipChangeDetected.store(true, std::memory_order_relaxed);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        // Snapshot guards a teardown race (see on_visual_equip_change).
        auto trampoline = s_originalVisualEquipSwap;
        if (!trampoline)
            return 0;
        return trampoline(a1, a2, a3, a4);
    }

    bool consume_equip_change() noexcept
    {
        return s_equipChangeDetected.exchange(false, std::memory_order_relaxed);
    }

} // namespace EquipHide
