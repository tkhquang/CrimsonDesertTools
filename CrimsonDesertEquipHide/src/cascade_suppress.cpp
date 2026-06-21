#include "cascade_suppress.hpp"
#include "categories.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

namespace EquipHide
{
    static std::atomic<bool> s_equipChangeDetected{false};

    // Slot ID for chest armor (confirmed via log: slot=4, item=5208). Only chest changes need cascade re-sync -- other
    // armor slots (necklace=9, mask=18, etc.) don't affect chest lock state.
    static constexpr uint16_t k_chestSlot = 4;

    // BatchEquip dispatch-entry layout used to read the per-entry slot id. Decoded live from the dispatch code so a
    // future entry re-widening self-corrects, with the last-known 240/216 layout as a validated fallback. A hardcoded
    // size silently mis-reads the slot id once the engine grows the entry (it widened 232/208 -> 240/216), which is why
    // the layout is decoded live rather than fixed.
    //
    // Stride: the entry array is indexed by `imul rsi, rax, imm32`. The AOB anchors on `mov rbx,[rcx]; mov eax,[rcx+8];
    // imul rsi,rax,imm32` and lands (+6) on the imul; its imm32 operand is the 240-byte entry stride. The pattern stops
    // before the imm32 so it stays value-agnostic (self-heals).
    static constexpr DMK::Scanner::AddrCandidate k_equipSwapStrideSite[] = {
        {"BatchEquipStride", "48 8B 19 8B 41 08 48 69 F0", DMK::Scanner::ResolveMode::Direct, 6, 0, true},
    };
    // Slot: the per-entry slot id is the word read by `movzx eax, word ptr [rbx+disp32]` immediately before `cmp word
    // ptr [rdx+disp32], ax`. The AOB wildcards the displacement (so a shifted slot still matches) and anchors on the
    // trailing cmp opcode; it lands (+0) on the movzx, whose [rbx+disp32] displacement is the +216 slot.
    static constexpr DMK::Scanner::AddrCandidate k_equipSwapSlotSite[] = {
        {"BatchEquipSlot", "0F B7 83 ?? ?? ?? ?? 66 39 82", DMK::Scanner::ResolveMode::Direct, 0, 0, true},
    };

    // Decode an instruction operand to a layout constant, validated to a plausible range. On any miss, out-of-range
    // value, or decode exception the nominal is kept, so a wrong anchor or operand index can never mis-read the
    // dispatch entry; the decoded value is logged once for verification.
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
                // A live value != nominal means the engine layout drifted on a patch; the decode self-healed it, but
                // surface it as a WARNING so the offset change is easy to spot in the log.
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
        static const std::size_t value = decode_layout_constant(
            k_equipSwapStrideSite, DMK::Scanner::OperandKind::Immediate, 2, 216, 256, 240, "stride");
        return value;
    }

    [[nodiscard]] static std::size_t equip_swap_slot_offset() noexcept
    {
        static const std::size_t value = decode_layout_constant(
            k_equipSwapSlotSite, DMK::Scanner::OperandKind::MemoryDisplacement, 1, 192, 224, 216, "slot");
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
        // detour, but a game thread already past the JMP might still enter the body before the DLL unmaps. Returning
        // zero matches the engine's own no-op shape for this slot-update API.
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
        // Warm the layout self-heal at install (setup/control-plane) so the dispatch-entry stride/slot are decoded and
        // cached before the first swap, keeping the hot path free of the one-time AOB scan.
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
