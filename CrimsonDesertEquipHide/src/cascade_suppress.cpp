#include "cascade_suppress.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    static std::atomic<bool> s_equipChangeDetected{false};

    // Slot id for chest armor. Only a chest change needs a cascade re-sync -- the other armor slots (necklace=9,
    // mask=18, and so on) do not affect the chest lock state.
    static constexpr uint16_t k_chestSlot = 4;

    // BatchEquip dispatch-entry layout, used to read the per-entry slot id.
    //
    // The engine walks two nested containers here, and the mod must read from the outer one. The outer loop iterates
    // the swap entries handed to on_visual_equip_swap: it loads the base from [rcx] and the count from [rcx+8], then
    // forms the end pointer with `imul rsi, rax, <stride>`. The inner loop searches one actor's authority table with a
    // different stride and a different slot field. A read of the inner values mis-indexes every entry, so the mod
    // decodes both constants below from the outer loop.
    //
    // The mod decodes both values live and does not hardcode them. The entry width changes across builds, so a fixed
    // size mis-reads the slot id silently instead of failing. The nominal is the last verified layout, and the code
    // uses it only when the decode fails. Verify it against live memory on patch day.
    //
    // Stride: the imm32 of the outer `imul rsi, rax, <stride>`. No candidate pins that imm32. P1 and P2 stop before
    // it, and P3 wildcards it. The pattern therefore stays value-agnostic and self-heals when the entry width changes.
    //
    // A short branch sits between the outer loop head and the imul, and the same three-instruction shape appears at an
    // unrelated container-iteration site. No branch-free window tells the two sites apart, so each candidate below
    // includes one short jump with a wildcarded target. If a future build changes that jump encoding, the candidate
    // stops matching. That result is intended: the decode falls back to the nominal instead of resolving to the wrong
    // site.
    static constexpr DMK::Scanner::AddrCandidate k_equipSwapStrideSite[] = {
        // P1 -- outer loop head through the imul. Widest context, anchored before the null check.
        {"BatchEquipStride_P1_LoopHeadToImul",
         "C7 85 ?? ?? ?? ?? ?? ?? ?? ?? 49 8B 09 48 85 C9 74 ?? 48 8B 19 8B 41 08 48 69 F0",
         DMK::Scanner::ResolveMode::Direct, 0x18, 0, true},

        // P2 -- null check through the imul. Drops the preceding frame initialization.
        {"BatchEquipStride_P2_NullCheckToImul", "49 8B 09 48 85 C9 74 ?? 48 8B 19 8B 41 08 48 69 F0",
         DMK::Scanner::ResolveMode::Direct, 14, 0, true},

        // P3 -- imul forward into the branch that follows it. Independent of everything before the imul, so it
        // survives a rewrite of the loop head that defeats P1 and P2.
        {"BatchEquipStride_P3_ImulToJoin",
         "48 8B 19 8B 41 08 48 69 F0 ?? ?? ?? ?? 48 03 F3 EB ?? 49 8B 41 08 48 8B 18",
         DMK::Scanner::ResolveMode::Direct, 6, 0, true},
    };

    // Slot: the disp32 of `movzx eax, word ptr [rbx+<slot>]`, where rbx is the current outer entry. The instruction
    // that follows compares that word against the inner table's own slot field, which uses a different offset. Anchor
    // on the movzx, not on the compare.
    static constexpr DMK::Scanner::AddrCandidate k_equipSwapSlotSite[] = {
        // P1 -- inner-loop setup through the movzx. Widest context and unique across the whole process.
        {"BatchEquipSlot_P1_InnerSetupToMovzx", "48 69 C8 ?? ?? ?? ?? 48 03 CA 48 3B D1 74 ?? 0F B7 83",
         DMK::Scanner::ResolveMode::Direct, 0x0F, 0, true},

        // P2 -- movzx and compare, extended into the entry advance that follows.
        {"BatchEquipSlot_P2_MovzxCompareAdvance", "0F B7 83 ?? ?? ?? ?? 66 39 82 ?? ?? ?? ?? 74 ?? 48 81 C2",
         DMK::Scanner::ResolveMode::Direct, 0, 0, true},

        // P3 -- movzx and compare only. Both displacements are wildcarded, so a shifted slot field still matches.
        {"BatchEquipSlot_P3_MovzxCompare", "0F B7 83 ?? ?? ?? ?? 66 39 82 ?? ?? ?? ??",
         DMK::Scanner::ResolveMode::Direct, 0, 0, true},
    };

    // Decode an instruction operand to a layout constant, validated to a plausible range. On any miss, out-of-range
    // value, or decode exception the code keeps the nominal. A wrong anchor or operand index therefore can never
    // mis-read the dispatch entry. The code logs the decoded value once for verification.
    [[nodiscard]] static std::size_t decode_layout_constant(std::span<const DMK::Scanner::AddrCandidate> site,
                                                            DMK::Scanner::OperandKind kind, std::uint8_t operandIndex,
                                                            std::int64_t lo, std::int64_t hi, std::size_t nominal,
                                                            const char *label) noexcept
    {
        try
        {
            DMK::Scanner::CodeConstant cc{};
            cc.site = site;
            cc.kind = kind;
            cc.operand_index = operandIndex;
            cc.nominal = static_cast<std::int64_t>(nominal);
            cc.has_nominal = true;
            const auto decoded = DMK::Scanner::read_code_constant(cc);
            if (decoded.has_value() && *decoded >= lo && *decoded <= hi)
            {
                const auto value = static_cast<std::size_t>(*decoded);
                // A live value != nominal means the engine layout drifted on a patch. The decode self-heals it, but
                // reports a WARNING so the offset change is easy to find in the log.
                if (value != nominal)
                    DMK::Logger::get_instance().warning(
                        "BatchEquip {} DRIFTED: live={} nominal={} -- self-healed (engine layout changed)", label,
                        value, nominal);
                else
                    DMK::Logger::get_instance().info("BatchEquip {} decoded live: {} (matches nominal)", label, value);
                return value;
            }
            DMK::Logger::get_instance().warning("BatchEquip {} live-decode out of range/unavailable; using nominal {}",
                                                label, nominal);
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
            k_equipSwapStrideSite, DMK::Scanner::OperandKind::Immediate, 2, 216, 264, 240, "stride");
        return value;
    }

    [[nodiscard]] static std::size_t equip_swap_slot_offset() noexcept
    {
        // Nominal is the slot field of the outer entry, 216 (0xD8). Do not set this to the inner table's slot field:
        // that one sits at a different offset and belongs to a container with a different stride.
        static const std::size_t value = decode_layout_constant(
            k_equipSwapSlotSite, DMK::Scanner::OperandKind::MemoryDisplacement, 1, 192, 240, 216, "slot");
        return value;
    }

    // --- VisualEquipChange hook (equip/unequip) ---

    static VisualEquipChangeFn s_originalVisualEquipChange = nullptr;

    void set_visual_equip_change_trampoline(VisualEquipChangeFn original)
    {
        s_originalVisualEquipChange = original;
    }

    __int64 __fastcall on_visual_equip_change(__int64 bodyComp, int16_t slotId, int16_t itemId, __int64 itemData)
    {
        DMK::Logger::get_instance().trace("VisualEquipChange: slot={} item={}", slotId, itemId);

        if (flag_cascade_fix().load(std::memory_order_relaxed) && is_category_hidden(Category::Chest) &&
            slotId == k_chestSlot)
        {
            DMK::Logger::get_instance().debug("VisualEquipChange: chest slot={} item={} -- clearing cascade locks",
                                              slotId, itemId);
            s_equipChangeDetected.store(true, std::memory_order_relaxed);
        }
        // Snapshot guards a teardown race: shutdown calls remove_hook() which restores the prologue and disables the
        // detour, but a game thread already past the JMP can still enter the body before the DLL unmaps. A return of
        // zero matches the engine no-op shape for this slot-update API.
        auto trampoline = s_originalVisualEquipChange;
        if (!trampoline)
            return 0;
        return trampoline(bodyComp, slotId, itemId, itemData);
    }

    // --- VisualEquipSwap hook (direct item-to-item swap) ---

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
            auto &logger = DMK::Logger::get_instance();
            __int64 *iter = a4 ? (*a4 ? *a4 : a4[1]) : nullptr;
            if (iter)
            {
                auto base = *iter;
                auto count = *reinterpret_cast<const uint32_t *>(reinterpret_cast<const char *>(iter) + 8);
                bool hasChest = false;
                for (uint32_t i = 0; i < count && i < 16; ++i)
                {
                    auto slot = *reinterpret_cast<const uint16_t *>(base + equip_swap_entry_stride() * i +
                                                                    equip_swap_slot_offset());
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
