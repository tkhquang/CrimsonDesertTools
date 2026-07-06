#include "transmog_apply.hpp"
#include "body_variant_hook.hpp"
#include "color_override/color_override.hpp"
#include "color_override/color_reinit.hpp"
#include "color_override/host_scope.hpp"
#include "dye_record_inject.hpp"
#include "color_override/setter_substitute.hpp"
#include "prefab_wrapper_swap.hpp"
#include "carrier_defaults.hpp"
#include "item_name_table.hpp"
#include "part_show_suppress.hpp"
#include "preset_manager.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog_hooks.hpp"
#include "transmog_map.hpp"
#include "transmog_worker.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>

namespace Transmog
{
    // SlotPopulator (sub_14076C960) maintains a dispatch cache on the component at (basePtr, count, cap). The triple
    // shifted 0x20 bytes higher between v1.03.01 and v1.04.00 as the component gained new fields lower in the struct;
    // in v1.08.00 the same sub-struct moved another 8 bytes higher (an extra qword was inserted below it):
    //
    //   v1.03.01 -- basePtr @ +0x1B8, count @ +0x1C0, cap @ +0x1C4
    //   v1.04.00 -- basePtr @ +0x1D8, count @ +0x1E0, cap @ +0x1E4
    //   v1.08.00 -- basePtr @ +0x1E0, count @ +0x1E8, cap @ +0x1EC
    //
    // Reading the old offsets on v1.04.00 returns zero for count (the u32 that lives there is always 0), which makes
    // apply look like a no-op; writing the old offsets scribbles into adjacent fields and corrupts the component a slot
    // at a time. On v1.08.00 the old count slot now overlaps the LOW 32 bits of the basePtr qword, so a stale read
    // yields a wildly inflated value (e.g. 0xA8000000 = 2819141376 -- the low bits of a 0x000004??A8XXXXXX heap
    // address) and any write to it would shred the dispatch-cache pointer.
    //
    // The v1.08 SlotPopulator body at 0x1407A5F60+0xDB confirms the new offsets directly:
    //   lea  r15, [r13+0x1E0]    ; r13 = a1
    //   mov  r8d, [r15+0x08]     ; count   = [a1+0x1E8]
    //   mov  r9,  [r15]          ; basePtr = [a1+0x1E0]
    constexpr std::ptrdiff_t k_compSlotCacheBasePtrOffset = 0x1E0;
    constexpr std::ptrdiff_t k_compSlotCacheCountOffset = 0x1E8;
    constexpr std::ptrdiff_t k_compSlotCacheCapOffset = 0x1EC;

    // Pointer to the PartDef/auth-table container on a1.
    //
    //   v1.03.01 -- container @ a1 + 0x78
    //   v1.04.00 -- container @ a1 + 0x88   (+0x10 of new fields below)
    //   v1.05.00 -- container @ a1 + 0x88   (unchanged from v1.04)
    //
    // Mirrors k_containerPtrOffset in real_part_tear_down.cpp. Reading the old offset on v1.04.00 yields a
    // non-container qword that either dereferences to junk or fails the >0x10000 sanity gate, skipping the real-item
    // restore loop entirely.
    constexpr std::ptrdiff_t k_compEntryTablePtrOffset = 0x88;

    // Entry layout within the auth-table array. The stride and slot-tag offset move together by 8 across versions:
    //
    //   v1.04.00: stride=0xC8 (200), slotTag@+0xC0
    //   v1.05.00: stride=0xD0 (208), slotTag@+0xC8
    //   v1.13.00: stride=0xC8 (200), slotTag@+0xC0  (reverted to the v1.04 layout)
    //
    // primary item id at +0x08 is unchanged. Slot-tag VALUES themselves are unchanged from v1.04; only the position
    // within the entry shifted. Mirrors k_entryStride / k_entrySlotTagOffset in real_part_tear_down.cpp.
    constexpr std::ptrdiff_t k_compEntryStride = 0xC8;
    constexpr std::ptrdiff_t k_compEntryItemIdOffset = 0x08;
    constexpr std::ptrdiff_t k_compEntrySlotTagOffset = 0xC0;

    // (TransmogSlot, engine slot tag) pairs the dispatcher iterates for tear-down + the auth-table real-id snapshot.
    // Sourced from slot_metadata.hpp's single per-slot table; the local TearDownSlot alias keeps existing call sites
    // (`td.slot`, `td.gameTag`) reading unchanged. Order matches the TransmogSlot enum.
    using TearDownSlot = SlotMetadata;
    static constexpr auto &k_tearDownSlots = k_slotMetadata;
    static constexpr std::size_t k_tearDownCount = k_slotCount;

    // Forward decls -- both functions are defined further down this TU. needs_carrier drives the carrier decision in
    // the apply paths; set_char_class_bypass is toggled by the per-slot tear-down bypass in apply_single_slot_transmog,
    // apply_all_transmog, and clear_all_transmog so a cross-class carrier fake stays reachable while it is torn down.
    bool needs_carrier(uint16_t itemId, const std::string &charName);
    static bool set_char_class_bypass(uintptr_t addr, uint8_t val) noexcept;

    // Walk the auth-table for the entry whose +0xC8 slotTag matches `gameTag`, snapshot its dye-record vector, and
    // publish via DyeRecordInject so the next apply_transmog -> SlotPopulator -> DyeCopier round-trip emits exactly
    // those records into the render struct's dst+120. Returns true when records were published; the caller is
    // responsible for clear_slot_dye_state after the apply pass.
    //
    // Used ONLY by the untick-restore branch in apply_all_transmog (`!m.active && prevIds != 0`): when the user unticks
    // a slot and the real item is coming back into view, repaint it in its current inventory dye instead of its factory
    // palette.
    //
    // Fakes with no explicit preset dye flow through with their natural engine records; monster-carrier fakes whose
    // engine source is empty render colorless. To seed preset dye for those fakes the user must explicitly call Capture
    // Outfit (mass) or the per-slot "Sync from live" button in the dye popup, which are the only paths that mutate the
    // active preset.
    static bool publish_entry_dye_for_gameslot(__int64 a1, std::int16_t gameTag) noexcept
    {
        uintptr_t entryBase = 0;
        __try
        {
            const auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + k_compEntryTablePtrOffset);
            if (entryDesc < 0x10000)
                return false;
            const auto entryArray = *reinterpret_cast<uintptr_t *>(entryDesc + 8);
            const auto entryCount = *reinterpret_cast<uint32_t *>(entryDesc + 16);
            for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
            {
                const auto base = entryArray + e * k_compEntryStride;
                const auto sl = *reinterpret_cast<int16_t *>(base + k_compEntrySlotTagOffset);
                if (sl == gameTag)
                {
                    entryBase = base;
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (entryBase == 0)
            return false;
        DyeRecordInject::ChannelState live[DyeRecordInject::k_dyeChannelCount];
        if (DyeRecordInject::read_entry_dye_records(entryBase, live) == 0)
            return false;
        const auto tmSlot = slot_from_game_tag(gameTag);
        DyeRecordInject::log_dye_snapshot("restore", tmSlot.has_value() ? slot_name(*tmSlot) : "?", live);
        // Sparse mode: emit only the channels that the auth-table entry actually carried. Dense fill would paint mesh
        // sub-parts (e.g. cloak facings) that the real item never colored, using the first active channel as fallback.
        DyeRecordInject::set_slot_dye_state(live, /*sparse=*/true);
        return true;
    }

    // Helper for the dye-pass-through path.
    //
    // Walks the per-actor item-instance table at *(a1+0x88)+0x08, which is the ROOT source the engine's secondary-id
    // resolver (sub_141B2F780) searches. Returns the first non-0xFFFF u16 found at byte +0xC8 of any 208-byte record.
    //
    // Per sub_141B2F780 decompile:
    //   v4 = *(QWORD*)(actor + 0x88)         // actor sub-system header
    //   v7 = *(WORD**)(v4 + 8)               // ptr to records array
    //   count = *(u32*)(v4 + 16)             // record count
    //   record stride: 104 WORDs = 208 bytes
    //   record + 200 (= 100 WORDs = +0xC8): u16 KEY (matched against query)
    //   record + 8 (= 4 WORDs):              u16 RESULT (returned variant id)
    //
    // The auth table at actor+472 was the WRONG source: LT's 0xFFFF sentinel propagates into it on every apply,
    // polluting the capture. The instance table at actor+0x88 is updated by the natural equip flow and unaffected by
    // LT.
    // Body-eligibility half of the bypass-skip decision (see should_skip_bypass): true when the target's body
    // restriction is compatible with the wearer -- it has no single-body restriction (Generic/dual, so the engine
    // decides), or its restricted body equals the wearer's. A Male-only item on Damiane (or a Female-only item on a
    // male character) is ineligible. Before the catalog resolves, report ineligible so the bypass stays on (fail-safe:
    // an unresolved item keeps the forced-render path).
    static bool char_eligible_for_target(uint16_t targetId, const std::string &charName)
    {
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
            return false;
        const auto itemBody = table.body_kind_for_item(targetId);
        if (itemBody != ItemNameTable::BodyKind::Male && itemBody != ItemNameTable::BodyKind::Female)
            return true;
        return itemBody == ItemNameTable::body_kind_for_character(charName);
    }

    // Descriptor offset of the per-body MESH variant table (0 for items without one). Boss/dual-body armor the 1.13
    // patch opened to a second body (Samuel/OrcuMer/Heisellen, ...) stores its {male mesh, female mesh, per-body token
    // groups} here; the engine walks it, keyed on the wearer's body token, to pick the mesh. The offset is
    // build-specific; re-confirm it if a patch reshuffles the item descriptor.
    static constexpr std::ptrdiff_t k_descBodyVariantTableOffset = 0x220;

    // The full bypass-skip decision: LT should NOT force the char-class bypass when the target carries a per-body mesh
    // variant table (desc+0x220) AND the wearer is body-eligible for it. Only those items suffer the bypass locking the
    // default (male) mesh; an item without a variant table (a plain accessory, or a cross-class NPC item) needs the
    // bypass to render and keeps it. Consulted when applying (skip the enable) and when tearing down (a fake applied
    // without the bypass is reachable normally; forcing it during tear-down would restore the real item on the wrong
    // body).
    static bool should_skip_bypass(uint16_t targetId, const std::string &charName)
    {
        if (!char_eligible_for_target(targetId, charName))
            return false;
        const uintptr_t desc = ItemNameTable::instance().descriptor_of(targetId);
        if (desc == 0)
            return false;
        return DMKMemory::seh_read<uintptr_t>(desc + k_descBodyVariantTableOffset).value_or(0) > 0x10000;
    }

    // Legacy fallback gate: force the char-class bypass for the whole apply window ONLY when BodyVariantHook is not live
    // (its resolver/render AOBs did not resolve, or its hooks failed to install). BodyVariantHook normally owns the
    // per-body mesh pick -- it observes the engine's natural match and forces entry[0] only for items the wearer cannot
    // equip. With the hook down that observation is gone, so LT falls back to the pre-hook behavior: force entry[0] via
    // the bypass so cross-class/NPC items still render (visible, default mesh) instead of turning invisible.
    // should_skip_bypass still excludes dual-body armor the wearer is eligible for, so that armor keeps its correct mesh
    // through the engine's own resolver; an unresolved catalog reports "do not skip" (fail-safe: keep the forced-render
    // path). Returns false when charClassBypass itself did not resolve -- there is no byte to flip.
    static bool legacy_bypass_force_needed(uint16_t targetId)
    {
        if (BodyVariantHook::is_active())
            return false;
        if (resolved_addrs().charClassBypass == 0)
            return false;
        return !should_skip_bypass(targetId, current_apply_owner());
    }

    // SlotPopulator choke point shared by the direct and carrier apply paths. `id` is the descriptor id fed to the
    // engine (the real target on the direct path, the carrier id on the carrier path, where the target visuals arrive
    // via the swapped hybrid descriptor). `forceBypass` requests the legacy char-class-bypass force for the duration of
    // the call and is decided by the caller from the REAL target (see legacy_bypass_force_needed); it is always false
    // while BodyVariantHook is live, since the hook then owns the per-body mesh pick.
    static void apply_transmog_core(__int64 a1, uint16_t id, bool forceBypass)
    {
        auto slotPop = slot_populator_fn();
        auto initEntry = init_swap_entry_fn();
        if (!slotPop || !initEntry)
            return;

        // Build 16-byte item data structure for SlotPopulator. Layout matches a natural-engine equip exactly:
        //   XX YY 02 00 00 00 00 00 FF FF FF FF FF FF 00 00
        // The engine validates the +4..+11 region as part of its dye/material-instance lookup; reordering those dwords
        // causes the engine to fall back to default colors even when the wrapper-swap mesh is correct.
        alignas(16) uint8_t itemData[16]{};
        *reinterpret_cast<uint16_t *>(itemData + 0) = id;
        itemData[2] = 2;
        // bytes 4..7 left as 0 (zero-init)
        *reinterpret_cast<uint32_t *>(itemData + 8) = 0xFFFFFFFF;
        *reinterpret_cast<uint16_t *>(itemData + 12) = 0xFFFF;

        // Build empty swap entry.
        alignas(16) uint8_t swapEntry[256]{};
        initEntry(reinterpret_cast<__int64>(swapEntry));

        // When BodyVariantHook is live it selects the per-body mesh (and force-renders entry[0] only for items the
        // wearer cannot equip), gated on the in_transmog() flag set here, and forceBypass is false. When it is down,
        // forceBypass holds the char-class bypass forced for the whole apply so cross-class/NPC items still render.
        const uintptr_t bypassAddr = forceBypass ? resolved_addrs().charClassBypass : 0;

        in_transmog().store(true, std::memory_order_relaxed);
        // Reset the host-scope cluster so the upcoming slotPop's matInst-iter hits build a fresh player-vs-NPC
        // histogram.
        ColorOverride::HostScope::begin_apply_window();
        // Open the setter-property substitute window. Any 4-byte material-property write the engine fires during
        // slotPop will be redirected to the user's chosen RGB. Closes again immediately after so unrelated render
        // passes aren't tinted.
        ColorOverride::SetterSubstitute::set_apply_window(true);
        if (bypassAddr)
            set_char_class_bypass(bypassAddr, 0xEB);
        // slotPop faults (structured exception) on early load before game data is ready. __finally restores the bypass
        // byte, the apply window, and the in_transmog() flag even on an SEH unwind: a stranded in_transmog() would make
        // BodyVariantHook rewrite the char-class bypass on real (non-transmog) equips, a stranded apply window would
        // tint unrelated render passes, and a stranded 0xEB would force entry[0] on every later resolve (reintroducing
        // the male-variant mis-render on all characters).
        __try
        {
            slotPop(a1, reinterpret_cast<unsigned __int16 *>(itemData), reinterpret_cast<__int64>(swapEntry));
        }
        __finally
        {
            if (bypassAddr)
                set_char_class_bypass(bypassAddr, 0x74);
            ColorOverride::SetterSubstitute::set_apply_window(false);
            in_transmog().store(false, std::memory_order_relaxed);
        }
    }

    void apply_transmog(__int64 a1, uint16_t targetId)
    {
        apply_transmog_core(a1, targetId, legacy_bypass_force_needed(targetId));
    }

    // -- Default carrier set (per character) -----------------------------
    // Each entry must be a valid item for THAT character in the given slot -- something the engine's equip class-gate
    // accepts. Kliff and
    // Oongka share Kliff_PlateArmor_* (Oongka's equip-type matches
    // Kliff's). Damiane has her own armor namespace (Demian_*) which
    // Kliff cannot wear, but the engine accepts them on Damiane even though they are marked PlayerSafe=no in the item
    // catalog. The PlayerSafe flag reflects Kliff compatibility only.
    //
    // If a name fails to resolve the slot falls back to direct equip (which may silently fail for NPC/variant items).

    // Per-character default carrier item-names live in carrier_defaults.hpp::k_carriers[character][slot].itemName.
    // ItemNameTable resolves each to a uint16_t carrier itemId at runtime. This function picks the right row for the
    // active character and falls back to Kliff if the character-specific entry isn't catalog-resident.
    uint16_t default_carrier_for_slot(TransmogSlot slot, const std::string &charName)
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= k_slotCount)
            return 0;
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
            return 0;

        const auto charOpt = carrier_char_from_name(charName);
        const auto cc = charOpt.value_or(CarrierChar::Kliff);

        const char *name = carrier_for(cc, slot).itemName;
        auto id = table.id_of(name);
        if (id.has_value())
            return *id;

        // Fallback: if a character-specific carrier name did not resolve (missing from catalog, renamed), try Kliff's
        // set.
        if (cc != CarrierChar::Kliff)
        {
            auto kliff = table.id_of(carrier_for(CarrierChar::Kliff, slot).itemName);
            return kliff.value_or(0);
        }
        return 0;
    }

    // Expected equip-type u16 (at desc+0x42) for the given playable character:
    //   Kliff   = 0x0004
    //   Oongka  = 0x0001
    //   Damiane = 0x0001
    // Unknown characters default to Kliff's value so legacy call paths keep their prior behaviour.
    static std::uint16_t expected_equip_type_for_char(const std::string &charName) noexcept
    {
        if (charName == "Damiane" || charName == "Oongka")
            return 0x0001;
        return 0x0004;
    }

    bool needs_carrier(uint16_t itemId, const std::string &charName)
    {
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
            return false;

        // Variant-meta items always need the carrier-patch path so the NPC variant descriptor gets swapped in for
        // visuals while a clean carrier supplies the rule list.
        if (table.has_variant_meta(itemId))
            return true;

        // Damiane and Oongka ALWAYS use the carrier-hybrid path. The engine's body/class-list filter rejects most
        // cross-body items at the class gate (e.g. Catfish/Madacus/Brimstone/
        // Hyena helms reject Oongka while Marni-Devotee and Oongka_PlateArmor_Helm_III pass), producing INVISIBLE
        // renders even when equip_type matches. Forcing the carrier path on every Damiane/Oongka apply uses the
        // charClassBypass + equip-type patch combo that was already proven to bypass both class and tribe checks. Kliff
        // keeps the cheaper direct-apply path because his items are the engine's reference set; only genuinely
        // cross-class targets (NPC variants, etc.) trigger the carrier path for him via the equip_type check below.
        if (charName == "Damiane" || charName == "Oongka")
            return true;

        // Equip-type mismatch: the item's +0x42 slot class doesn't match what the active character's inventory slot
        // accepts. Without the carrier path the engine rejects the item at the equip gate (Oongka picking Kliff's
        // 0x0004 armor, Kliff picking Damiane's 0x0001 armor, etc.). The hybrid patches the carrier's equip-type over
        // the target descriptor so the slot accepts while the target's visuals render.
        const std::uint16_t expected = expected_equip_type_for_char(charName);
        const std::uint16_t actual = table.equip_type_of(itemId);
        // equip_type_of returns 0 if the catalog hasn't resolved this id yet; fall back to the old broad
        // "is_player_compatible" gate in that case so we don't silently direct-apply a cross-class item during the
        // deferred-scan window.
        if (actual == 0)
            return !table.is_player_compatible(itemId) || table.has_npc_equip_type(itemId);

        return actual != expected;
    }

    // Legacy wrapper retained for any call sites that still assume
    // Kliff. Defers to the per-character overload with Kliff bound.
    bool needs_carrier(uint16_t itemId)
    {
        return needs_carrier(itemId, std::string("Kliff"));
    }

    // Descriptor size. Stride between consecutive descriptors observed in CE on the live build:
    //
    //   v1.03.01 -- 0x400 (1024)
    //   v1.04.00 -- 0xA00 (2560)
    //
    // The hybrid buffer is memcpy'd from the target descriptor up to this size; any byte beyond ends up as
    // VirtualAlloc-zero. If SlotPopulator or its callees read past the copied range they see zeros instead of the real
    // descriptor data, which faults when the zero is treated as an embedded pointer, vtable, or index. Keep this at or
    // above the live stride; over-copy is safe because the source descriptor is always at least a full stride wide in
    // the pool allocator.
    static constexpr std::size_t k_descBufSize = 0xA00;

    // Descriptor offsets read by SlotPopulator for character-context matching BEFORE visual/mesh data. Must come from
    // the CARRIER so
    // Kliff's evaluator matches; everything else comes from TARGET.
    //
    // +0x042  2B  equip-type u16 (Kliff=4, NPC=1)
    //             SlotPopulator reads *(desc+0x42) for the visual-config
    //             matching loop (sub_14076CAED).
    struct PatchRange
    {
        std::ptrdiff_t off;
        std::size_t len;
    };
    static constexpr PatchRange k_carrierPatches[] = {
        {0x042, 0x002}, // equip-type u16 (Kliff=4, NPC=1)
    };

    // Write the char-class bypass byte unconditionally. No from-check:
    // the read-after-write can see stale values when page protection is restored between enable/restore cycles.
    static bool set_char_class_bypass(uintptr_t addr, uint8_t val) noexcept
    {
        if (!addr)
            return false;
        const auto byteVal = static_cast<std::byte>(val);
        return DMK::Memory::write_bytes(reinterpret_cast<std::byte *>(addr), &byteVal, 1).has_value();
    }

    // Swaps the carrier slot's descriptor pointer to the hybrid, calls SlotPopulator via apply_transmog_core, then
    // unconditionally restores the descriptor via __finally. The per-body mesh selection is handled by BodyVariantHook
    // during the apply when it is live; `forceBypass` (decided by the caller from the REAL target, not the carrier)
    // drives the legacy bypass-force fallback for the carrier path when the hook is down.
    //
    // __finally (not __except): runs cleanup WITHOUT catching the exception. Game-internal SEH exceptions (lazy
    // loading, page faults) continue propagating to the game's own handlers. __except would intercept them and break
    // the game. This matters because apply_transmog_core faults on early load attempts (game data not ready); without
    // __finally the swapped descriptor pointer would be left corrupted.
    static void carrier_swap_and_call(__int64 a1, uint16_t carrierId, uintptr_t carrierSlotAddr, uintptr_t hybridAddr,
                                      bool forceBypass) noexcept
    {
        const auto savedDesc = static_cast<LONG64>(InterlockedExchange64(
            reinterpret_cast<volatile LONG64 *>(carrierSlotAddr), static_cast<LONG64>(hybridAddr)));

        __try
        {
            apply_transmog_core(a1, carrierId, forceBypass);
        }
        __finally
        {
            InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(carrierSlotAddr), savedDesc);
        }
    }

    void apply_transmog_with_carrier(__int64 a1, uint16_t carrierId, uint16_t targetId)
    {
        auto &logger = DMK::Logger::get_instance();
        const auto &table = ItemNameTable::instance();

        if (carrierId == 0 || carrierId == targetId)
        {
            logger.trace("[carrier] carrierId={:#06x} == targetId={:#06x}, direct apply", carrierId, targetId);
            apply_transmog(a1, targetId);
            return;
        }

        const auto ci = table.catalog_info();
        if (ci.ptrArray == 0 || ci.count == 0)
        {
            logger.warning("[carrier] catalog not ready, direct fallback");
            apply_transmog(a1, targetId);
            return;
        }
        if (carrierId >= ci.count || targetId >= ci.count)
        {
            logger.warning("[carrier] id out of range: "
                           "carrier={:#06x} target={:#06x} count={}",
                           carrierId, targetId, ci.count);
            apply_transmog(a1, targetId);
            return;
        }

        const auto carrierSlotAddr = ci.ptrArray + static_cast<uint64_t>(carrierId) * 8;
        const uintptr_t targetDesc =
            DMKMemory::seh_read<uintptr_t>(ci.ptrArray + static_cast<uint64_t>(targetId) * 8).value_or(0);
        const uintptr_t carrierDesc = DMKMemory::seh_read<uintptr_t>(carrierSlotAddr).value_or(0);

        if (targetDesc < 0x10000 || carrierDesc < 0x10000)
        {
            logger.warning("[carrier] bad descriptor: "
                           "carrier={:#06x}={:#018x} target={:#06x}={:#018x}",
                           carrierId, carrierDesc, targetId, targetDesc);
            apply_transmog(a1, targetId);
            return;
        }

        // -- Build hybrid descriptor on the heap -------------------------
        // Must use VirtualAlloc so the address is in the high heap range (game may reject low stack addresses in
        // pointer sanity checks). Base = target's visual data. Overlay = carrier's matching fields.
        auto *hybrid =
            static_cast<uint8_t *>(VirtualAlloc(nullptr, k_descBufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!hybrid)
        {
            logger.warning("[carrier] VirtualAlloc failed for hybrid buffer");
            apply_transmog(a1, targetId);
            return;
        }

        if (!DMKMemory::seh_read_bytes(targetDesc, hybrid, k_descBufSize))
        {
            logger.warning("[carrier] fault copying target descriptor");
            VirtualFree(hybrid, 0, MEM_RELEASE);
            apply_transmog(a1, targetId);
            return;
        }

        // Overlay carrier fields used for character-context matching.
        std::size_t patchedBytes = 0;
        for (const auto &p : k_carrierPatches)
        {
            if (p.off + p.len > k_descBufSize)
                continue;
            if (!DMKMemory::seh_read_bytes(carrierDesc + p.off, hybrid + p.off, p.len))
            {
                logger.warning("[carrier] fault patching carrier field "
                               "at +{:#x} len={}",
                               p.off, p.len);
                VirtualFree(hybrid, 0, MEM_RELEASE);
                apply_transmog(a1, targetId);
                return;
            }
            patchedBytes += p.len;
        }

        const auto hybridAddr = reinterpret_cast<uintptr_t>(hybrid);

        logger.trace("[carrier] HYBRID built: {} bytes patched, "
                     "carrier={:#06x} desc={:#018x}, "
                     "target={:#06x} desc={:#018x}, hybrid={:#018x}",
                     patchedBytes, carrierId, carrierDesc, targetId, targetDesc, hybridAddr);

        // SEH-isolated: swap the carrier descriptor to the hybrid, call SlotPopulator, then restore. The per-body mesh
        // pick is handled by BodyVariantHook when it is live; forceBypass drives the legacy fallback for the REAL target
        // (not the carrier) when it is down.
        const bool forceBypass = legacy_bypass_force_needed(targetId);
        carrier_swap_and_call(a1, carrierId, carrierSlotAddr, hybridAddr, forceBypass);

        logger.trace("[carrier] POST: carrier={:#06x} target={:#06x}", carrierId, targetId);

        VirtualFree(hybrid, 0, MEM_RELEASE);
    }

    // -- Single-slot apply ----------------------------------------------
    //
    // Scoped version of apply_all_transmog that only touches ONE slot. Used by hover-preview to avoid the full
    // tear-down + re-apply of all 5 slots, which causes visible flicker on unchanged gear.
    //
    // Differences from apply_all_transmog:
    //   - Dispatch cache: only clears entries matching this slot's game tag (by walking the 24-byte stride array). Does
    //     NOT reset the global count to zero -- other slots' entries stay valid.
    //   - Tear-down: Phase A (previous fake) and Phase B (real item)
    //     scoped to one slot.
    //   - Apply: single call to apply_transmog or
    //     apply_transmog_with_carrier.
    //   - State: only updates lastIds, carrier, suppress for this slot.

    void apply_single_slot_transmog(__int64 a1, std::size_t slotIdx)
    {
        if (slotIdx >= k_slotCount)
            return;

        auto &logger = DMK::Logger::get_instance();
        auto &mappings = slot_mappings();
        auto &lastIds = last_applied_ids();
        auto &m = mappings[slotIdx];

        // resolve_player_component() walks WorldSystem -> ActorManager
        // -> UserActor -> actor and always returns Kliff's component
        // regardless of the currently-controlled character. Using it unconditionally clobbers per-character a1 values
        // from VEC / BatchEquip hooks. Keep it ONLY as a fallback when the passed-in a1 is invalid.
        if (a1 < 0x10000 && world_system_ptr().load(std::memory_order_acquire))
        {
            auto fresh = resolve_player_component();
            if (fresh > 0x10000)
                a1 = fresh;
        }

        __try
        {
            auto actor = *reinterpret_cast<uintptr_t *>(a1 + 8);
            if (actor < 0x10000)
            {
                logger.warning("apply_single_slot: a1 invalid");
                return;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("apply_single_slot: a1 access fault");
            return;
        }

        suppress_vec().store(true, std::memory_order_release);

        const uint16_t prevId = lastIds[slotIdx];
        const uint16_t gameTag = static_cast<uint16_t>(k_slotMetadata[slotIdx].gameTag);

        // Compute target: active slot with non-zero id -> transmog, otherwise clear this slot.
        const uint16_t targetId = (m.active && m.targetItemId != 0) ? m.targetItemId : 0;

        // One-shot force flag set by the body-mesh picker when re-picking a prefab on the same carrier id. Bypasses the
        // equality early-out so Phase A still runs against the real prevId, driving the engine's natural-pipeline hook
        // to clean up the prior tgt wrapper. Read-and-clear.
        const bool forceApply = force_apply_pending()[slotIdx];
        if (forceApply)
            force_apply_pending()[slotIdx] = false;

        // Early-out: nothing changed for this slot.
        if (targetId == prevId && targetId != 0 && !forceApply)
        {
            logger.trace("apply_single_slot: slot={} id={:#06x} unchanged", slotIdx, targetId);
            suppress_vec().store(false, std::memory_order_release);
            return;
        }

        // --- Scoped dispatch cache clear ---
        // Walk the 24-byte stride cache and zero subCount only for entries whose slotNativeId matches our game tag.
        // This leaves other slots' blobs untouched so VEC doesn't re-dispatch them.
        __try
        {
            const auto count = *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
            const auto base = *reinterpret_cast<volatile uintptr_t *>(a1 + k_compSlotCacheBasePtrOffset);
            if (base > 0x10000)
            {
                for (uint32_t e = 0; e < count; ++e)
                {
                    const auto entry = base + 24ULL * e;
                    const auto slotId = *reinterpret_cast<volatile uint16_t *>(entry);
                    if (slotId == gameTag)
                        *reinterpret_cast<volatile uint32_t *>(entry + 0x10) = 0;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("[dispatch] single-slot cache clear fault");
            suppress_vec().store(false, std::memory_order_release);
            return;
        }

        // --- Tear-down scoped to this slot ---
        std::uint16_t realId = 0;
        if (RealPartTearDown::is_ready())
        {
            realId = RealPartTearDown::get_real_item_id(reinterpret_cast<void *>(a1), gameTag);

            // Phase A: tear down previous fake. Runs even when the previous fake itemId matches the live real item; we
            // treat fake and real the same so the tear-down/apply sequence is always complete.
            if (prevId != 0)
            {
                const auto prevCarrier = last_applied_carrier_ids()[slotIdx];
                const uintptr_t bypassAddr = resolved_addrs().charClassBypass;
                // Only a carrier fake the wearer could NOT equip needs the bypass flipped on to be reachable; forcing
                // it across a body-eligible fake's tear-down would restore its real item on the wrong (default) body.
                // Matches the gate in apply_all_transmog / clear_all_transmog Phase A.
                const bool needBypass =
                    prevCarrier != 0 && !should_skip_bypass(static_cast<uint16_t>(prevId), current_apply_owner());
                const bool bypassApplied = needBypass && set_char_class_bypass(bypassAddr, 0xEB);

                if (prevCarrier != 0 && prevCarrier != static_cast<uint16_t>(prevId))
                {
                    RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), prevCarrier, gameTag);
                }
                RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), static_cast<uint16_t>(prevId),
                                                       gameTag);

                if (bypassApplied)
                    set_char_class_bypass(bypassAddr, 0x74);
            }

            // Phase B: tear down the real item. Runs unconditionally (fake == real is treated the same as fake !=
            // real).
            if (targetId != 0)
            {
                if (RealPartTearDown::tear_down_real_part(reinterpret_cast<void *>(a1), gameTag))
                    real_damaged()[slotIdx] = true;
            }
        }

        // --- Apply ---
        if (targetId != 0)
        {
            const auto tmSlot = static_cast<TransmogSlot>(slotIdx);
            // Use current_apply_owner so a targeted-apply onto a non-controlled body resolves its carrier from THAT
            // body's defaults. PresetManager::active_character() would return the controlled character, which under
            // pin+flag is the wrong axis -- carrier mismatch installs the wrong wrapper family on the body and produces
            // visual cross-talk through the swap map.
            const auto &activeChar = current_apply_owner();
            const bool useCarrier = needs_carrier(targetId, activeChar);
            const uint16_t carrierId = useCarrier ? default_carrier_for_slot(tmSlot, activeChar) : 0;

            // Same dye plumbing as apply_all_transmog. Without this, single-slot apply (manual_apply_slot from the dye
            // picker) bypasses the injector and the engine's natural records dominate.
            const Preset *activePreset = PresetManager::instance().active_preset();
            const SlotDyeChannels *slotDye =
                (activePreset && slotIdx < activePreset->slots.size()) ? &activePreset->slots[slotIdx].dye : nullptr;
            if (slotDye && any_dye_active(*slotDye))
            {
                DyeRecordInject::ChannelState state[DyeRecordInject::k_dyeChannelCount];
                for (std::size_t k = 0; k < DyeRecordInject::k_dyeChannelCount; ++k)
                {
                    const auto &ch = (*slotDye)[k];
                    state[k] = {ch.group_hash, ch.r, ch.g, ch.b, ch.material_id, ch.repair_byte};
                }
                const bool sparse = activePreset != nullptr && slotIdx < activePreset->slots.size() &&
                                    activePreset->slots[slotIdx].dyeSparse;
                DyeRecordInject::set_slot_dye_state(state, sparse);
            }
            else
            {
                // Preset has no explicit dye for this slot -- let the engine's natural dye records for the fake itself
                // flow through unmodified. clear_slot_dye_state() makes DyeRecordInject's post-trampoline detour skip
                // injection, so DyeCopier's natural copy of the fake's own records wins. Monster-carrier fakes with
                // empty engine source records will therefore render colorless; users must seed preset dye via Capture
                // Outfit or the per-slot "Sync from live" button to colour those.
                DyeRecordInject::clear_slot_dye_state();
            }

            ColorOverride::SetterSubstitute::set_active_slot(static_cast<int>(slotIdx));
            // Notify ColorOverride that this slot is being applied to the user-INTENDED target item. Wipes the swatch
            // table only when the user's chosen transmog target ACTUALLY changes -- not when the resolved target
            // temporarily flips to the carrier during an untick (which the dispatch path does).
            //
            // Pass the picked target when slot is active, 0 when unticked. Matches notify_transmog_target's contract:
            // 0 = "no transmog this slot", non-zero = the fake target the user wants to wear. Wipe fires only on
            // (non-zero last) -> (different non-zero new).
            {
                auto &mapping = slot_mappings()[slotIdx];
                const std::uint32_t userIntent = mapping.active ? static_cast<std::uint32_t>(mapping.targetItemId) : 0u;
                ColorOverride::Reinit::notify_transmog_target(static_cast<int>(slotIdx), userIntent);
            }
            if (useCarrier && carrierId != 0)
            {
                logger.debug("apply_single_slot: slot={} target={:#06x} "
                             "carrier={:#06x}",
                             slot_name(tmSlot), targetId, carrierId);
                apply_transmog_with_carrier(a1, carrierId, targetId);
            }
            else
            {
                logger.debug("apply_single_slot: slot={} target={:#06x}", slot_name(tmSlot), targetId);
                apply_transmog(a1, targetId);
            }

            DyeRecordInject::clear_slot_dye_state();

            lastIds[slotIdx] = targetId;
            last_applied_carrier_ids()[slotIdx] = (useCarrier && carrierId != 0) ? carrierId : 0;
            // Phase B set real_damaged when tearing down the real item for this slot. Now that the fake is successfully
            // applied, clear it so apply_all_transmog doesn't see stale damage state for this slot on subsequent
            // cycles.
            real_damaged()[slotIdx] = false;
        }
        else
        {
            // Clearing this slot. Two cases:
            //  - active + none (checkbox ticked, picker = none): user wants to show an EMPTY slot (bare head, etc.). Do
            //    NOT restore the real item -- Phase B already tore it down.
            //  - inactive (!m.active): slot was previously controlled by us; restore the real item so it reappears.
            const bool showEmpty = m.active;
            // During a 3-pass reinit cycle, suppress the real-armor restore so the slot goes visibly empty between
            // teardown and retick instead of flashing the real armor on every cycle.
            const bool reinitActive = ColorOverride::Reinit::is_slot_reinit_active(static_cast<int>(slotIdx));
            if (!showEmpty && (prevId != 0 || real_damaged()[slotIdx]) && !reinitActive)
            {
                if (realId != 0)
                {
                    logger.debug("apply_single_slot: slot={} restoring "
                                 "real {:#06x}",
                                 slot_name(static_cast<TransmogSlot>(slotIdx)), realId);
                    ColorOverride::SetterSubstitute::set_active_slot(static_cast<int>(slotIdx));
                    apply_transmog(a1, realId);
                }
            }
            else if (reinitActive)
            {
                logger.debug("apply_single_slot: slot={} real-restore SKIPPED "
                             "(reinit teardown -- slot goes empty by design)",
                             slotIdx);
            }
            lastIds[slotIdx] = 0;
            last_applied_carrier_ids()[slotIdx] = 0;
            // Clear damage flag so the slot is fully released back to the game. Without this, apply_all_transmog's
            // untick-restore and slotNeedsWork checks see stale damage state and keep interfering with an unmanaged
            // slot.
            real_damaged()[slotIdx] = false;
        }

        // Update suppress mask for this slot only. Rebuild full mask from current state rather than toggling one bit,
        // to stay consistent with apply_all_transmog's mask logic.
        std::uint32_t suppressMask = 0;
        for (std::size_t k = 0; k < k_slotCount; ++k)
        {
            const auto &sm = mappings[k];
            if (!sm.active)
                continue;
            const std::uint16_t slotReal =
                RealPartTearDown::is_ready()
                    ? RealPartTearDown::get_real_item_id(reinterpret_cast<void *>(a1),
                                                         static_cast<std::uint16_t>(k_slotMetadata[k].gameTag))
                    : 0;
            if (sm.targetItemId != 0 && static_cast<uint16_t>(sm.targetItemId) == slotReal)
                continue;
            suppressMask |= (std::uint32_t{1} << k);
        }
        PartShowSuppress::set_mask(suppressMask);

        logger.trace("apply_single_slot: slot={} done, suppress={:#x}", slotIdx, suppressMask);

        suppress_vec().store(false, std::memory_order_release);
    }

    void apply_all_transmog(__int64 a1)
    {
        auto &logger = DMK::Logger::get_instance();
        // Local copy of slot_mappings so we can synthesize an "all-slots-cleared" view when the user has toggled LT off
        // without mutating the persisted preset state. See the flag_enabled() block immediately below.
        auto mappings = slot_mappings();
        auto &lastIds = last_applied_ids();

        // Toggling Enabled off used to early-out of the dispatcher, which froze the cleanup pass (k_tearDownSlots loop
        // with mappings[idx].active==false + realItemId==0 + last_applied_real_ids[idx]!=0). Stale restore meshes from
        // a prior LT apply then leaked through the next organic radial unequip. Instead, force every mapping inactive
        // in this local copy and let the dispatcher run as if the user had unticked every slot. The cleanup pass
        // continues to tear down stale fakes; the apply pass writes nothing because every slot has active==false.
        //
        // The BE/VEC hooks (transmog_hooks.cpp) intentionally no longer gate on flag_enabled for the same reason --
        // they need to keep scheduling apply_all so this pass fires on every equip/unequip while disabled.
        if (!flag_enabled().load(std::memory_order_relaxed))
        {
            for (auto &m : mappings)
            {
                m.active = false;
                m.targetItemId = 0;
            }
        }

        // Fallback only -- see apply_single_slot_transmog comment.
        if (a1 < 0x10000 && world_system_ptr().load(std::memory_order_acquire))
        {
            auto fresh = resolve_player_component();
            if (fresh > 0x10000)
                a1 = fresh;
        }

        __try
        {
            auto actor = *reinterpret_cast<uintptr_t *>(a1 + 8);
            if (actor < 0x10000)
            {
                logger.warning("apply_all_transmog: a1 invalid");
                return;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("apply_all_transmog: a1 access fault");
            return;
        }

        // Engine-readiness gate. is_world_ready() observes the world singleton, which becomes non-null well before the
        // per-actor sub-handler at CCC+0x130 is wired. When the dispatcher runs against an actor whose sub-handler is
        // still null, the engine path under tear_down / tear_down_fake dereferences it as `v15 = *(v7 + 304); *v15;`,
        // the per-slot SEH wrappers burst-catch the resulting faults, every slot exits early, and the carrier apply
        // continues against a half-wired actor before raising out of the outer __try. Gating at this entry point covers
        // every caller (manual_apply, the multi-protagonist worker path, any future trigger) without coupling them to
        // LT-specific readiness semantics.
        constexpr std::uint64_t k_applyReadyRetryMs = 1000;
        if (!RealPartTearDown::is_actor_apply_ready(reinterpret_cast<void *>(a1)))
        {
            logger.debug("apply_all_transmog: actor not ready (a1={:#018x}), "
                         "re-arming in {} ms",
                         static_cast<uint64_t>(a1), k_applyReadyRetryMs);
            schedule_transmog_ms(k_applyReadyRetryMs);
            return;
        }

        // Suppress VEC hook for the entire operation.
        suppress_vec().store(true, std::memory_order_release);

        logger.trace("[dispatch] apply_all_transmog entry a1={:#018x}", static_cast<uint64_t>(a1));

        // Snapshot lastIds for diagnostic logging.
        const std::array<uint16_t, k_slotCount> prevIds = lastIds;

        // One-shot per-slot "force apply" snapshot (read-and-clear). Set by the body-mesh picker when the user re-picks
        // a prefab on the same carrier id (e.g. 0x1521 -> 0x1521 with a different src->tgt wrapper map). Without this
        // signal the dispatcher would see `wouldBe == prevIds[i]` and skip both presetChanged AND slotNeedsWork, which
        // means Phase A `tear_down_fake` never runs for the slot and the engine's natural-pipeline hook never gets
        // driven to clean up the prior tgt wrapper. With this set the dispatcher behaves as if the slot's preset had
        // genuinely changed, while leaving prevIds[i] intact so Phase A still tears down the prior carrier.
        std::array<bool, k_slotCount> forceApply{};
        {
            auto &fa = force_apply_pending();
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                forceApply[i] = fa[i];
                fa[i] = false;
            }
        }

        // Early-out: skip all work if neither the preset nor the real armor has changed since the last successful
        // apply. Drops spurious re-apply cycles fired by BatchEquip/VEC for non-armor events (weapon swaps, ring
        // changes, etc.).
        //
        // liveRealIds is indexed by TransmogSlot enum value (0..k_slotCount-1) and populated by walking k_tearDownSlots
        // which already enumerates every supported slot with its engine tag. Slots LT does not manage stay zeroed.
        std::array<std::uint16_t, k_slotCount> liveRealIds{};
        if (RealPartTearDown::is_ready())
        {
            for (const auto &td : k_tearDownSlots)
            {
                const auto idx = static_cast<std::size_t>(td.slot);
                liveRealIds[idx] = RealPartTearDown::get_real_item_id(reinterpret_cast<void *>(a1), td.gameTag);
            }
        }

        std::array<bool, k_slotCount> slotNeedsWork{};
        {
            bool presetChanged = false;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                const auto &m = mappings[i];
                const uint16_t wouldBe = (m.active && m.targetItemId != 0) ? m.targetItemId : 0;
                if (wouldBe != prevIds[i] || forceApply[i])
                {
                    presetChanged = true;
                    break;
                }
            }

            bool realChanged = false;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (liveRealIds[i] != last_applied_real_ids()[i])
                {
                    realChanged = true;
                    break;
                }
            }

            // Check for active "none" slots -- these need Phase B tear-down and suppress reinforcement even when
            // nothing else changed (the game may re-equip real items after our initial tear-down during the load
            // sequence).
            bool hasActiveNone = false;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (mappings[i].active && mappings[i].targetItemId == 0)
                {
                    hasActiveNone = true;
                    break;
                }
            }

            if (!presetChanged && !realChanged && !hasActiveNone)
            {
                // Trivially-destructible char buffers -- std::string here would violate SEH/object-unwinding (the
                // function contains __try frames). 8 chars per slot ("0x1234,") x 20 slots = 160 + slack.
                char prevBuf[256];
                char realBuf[256];
                std::size_t pOff = 0;
                std::size_t rOff = 0;
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    const int np = std::snprintf(prevBuf + pOff, sizeof(prevBuf) - pOff, "%s0x%04x", i ? "," : "",
                                                 static_cast<unsigned>(prevIds[i]));
                    if (np > 0)
                        pOff += static_cast<std::size_t>(np);
                    const int nr = std::snprintf(realBuf + rOff, sizeof(realBuf) - rOff, "%s0x%04x", i ? "," : "",
                                                 static_cast<unsigned>(liveRealIds[i]));
                    if (nr > 0)
                        rOff += static_cast<std::size_t>(nr);
                }
                logger.trace("apply_all_transmog: no state change "
                             "(prev=[{}] real=[{}]), skipping",
                             prevBuf, realBuf);
                suppress_vec().store(false, std::memory_order_release);
                return;
            }

            if (!presetChanged && realChanged)
            {
                char oldBuf[256];
                char newBuf[256];
                std::size_t oOff = 0;
                std::size_t nOff = 0;
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    const int no = std::snprintf(oldBuf + oOff, sizeof(oldBuf) - oOff, "%s0x%04x", i ? "," : "",
                                                 static_cast<unsigned>(last_applied_real_ids()[i]));
                    if (no > 0)
                        oOff += static_cast<std::size_t>(no);
                    const int nn = std::snprintf(newBuf + nOff, sizeof(newBuf) - nOff, "%s0x%04x", i ? "," : "",
                                                 static_cast<unsigned>(liveRealIds[i]));
                    if (nn > 0)
                        nOff += static_cast<std::size_t>(nn);
                }
                logger.debug("apply_all_transmog: real item changed, re-applying "
                             "(real=[{}] -> [{}])",
                             oldBuf, newBuf);

                // Real swap means any previously-damaged slot now has a NEW real item that is NOT damaged yet. Clear
                // the damage flags for slots whose real id changed so the fake==real skip works correctly for the new
                // real.
                for (std::size_t i = 0; i < k_slotCount; ++i)
                {
                    if (liveRealIds[i] != last_applied_real_ids()[i])
                        real_damaged()[i] = false;
                }
            }

            // Per-slot "needs work" flags. Computed BEFORE overwriting last_applied_real_ids so we compare against the
            // previous state. A slot needs tear-down + re-apply if its preset target changed OR its underlying real
            // item changed. Unchanged slots are skipped -- no tear-down, no cache clear, no SlotPopulator call -- so
            // they don't flicker.
            //
            // Unticked slots whose real changes ARE still marked: a prior restore via SlotPopulator may have left a
            // dispatch cache entry and scene-graph mesh. The cleanup pass after the untick-restore loop relies on
            // slotNeedsWork to find and tear down these stale entries.
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                const auto &m = mappings[i];
                const uint16_t wouldBe = (m.active && m.targetItemId != 0) ? m.targetItemId : uint16_t{0};
                if (wouldBe != prevIds[i] || forceApply[i])
                {
                    slotNeedsWork[i] = true;
                    continue;
                }

                // Check if the real item changed for this slot. Unticked slots still need cache cleanup when their real
                // changes -- an earlier restore via SlotPopulator may have left a dispatch entry that the game's own
                // unequip flow can't remove. liveRealIds and last_applied_real_ids are now both indexed by TransmogSlot
                // (k_slotCount-wide) so the comparison is direct.
                if (liveRealIds[i] != last_applied_real_ids()[i])
                    slotNeedsWork[i] = true;
                // Active "none" slots always need work for suppress reinforcement.
                if (m.active && m.targetItemId == 0)
                    slotNeedsWork[i] = true;
            }

            // Master enable mask. Disabled slots (multi-prefab non-armor and duplicate-tag slots -- see
            // SlotMetadata::enabled doc-block in slot_metadata.hpp) never participate in the dispatch, even if a preset
            // loaded them with active=true. This is the single defensive gate covering preset load, legacy presets
            // saved before disabling, and any future path that toggles `mappings[i].active`.
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (!slot_enabled(i))
                    slotNeedsWork[i] = false;
            }

            // NOTE: last_applied_real_ids is updated at the END of the function, after all applies succeed. If we crash
            // mid-apply (e.g. reload SEH), the old values remain so the next retry detects the real-armor change and
            // tries again.
        }

        // Clear lastIds for slots without a new target.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            auto &m = mappings[i];
            if (!m.active || m.targetItemId == 0)
                lastIds[i] = 0;
        }

        // SlotPopulator (sub_14076C960) maintains a dispatch cache on the component at (basePtr, count, cap). The
        // triple offsets are defined by the k_compSlotCache* constants at the top of this file (they shifted between
        // v1.03.01 and v1.04.00). Each entry is 24 bytes:
        //   +0x00 uint16  slotNativeId
        //   +0x08 __int128* subArray (queued ItemInfoBlobs)
        //   +0x10 uint32  subCount   (append-only, NEVER reset by game)
        //   +0x14 uint32  subCap
        //
        // Two hazards:
        //  (1) Stale entries survive across presets. We used to save/
        //      restore count, which re-exposed previous-preset entries
        //      and caused SlotPopulator's linear search (14076ca7b) to
        //      find an old slotNativeId and *append* a new blob to its
        //      existing subArray -- the stale blob still gets dispatched
        //      to VEC via sub_14076D520, producing the stale helm bug.
        //  (2) Even on a slot we ARE re-populating this frame, the game
        //      never clears subCount, so the previously queued blob
        //      lingers and gets replayed alongside the new one.
        //
        // Fix: only clear subCount for dispatch cache entries whose slotNativeId matches a slot we are about to
        // re-apply. This avoids nuking unchanged slots' blobs, which would force the game to re-dispatch them through
        // VEC and cause visible flicker on gear that hasn't changed.
        //
        // Build a set of game tags that need clearing: any active slot with a non-zero target, plus any unticked slot
        // that needs real-item restoration.
        std::uint16_t clearTags[k_slotCount]{};
        std::size_t clearTagCount = 0;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (slotNeedsWork[i])
                clearTags[clearTagCount++] = static_cast<std::uint16_t>(k_slotMetadata[i].gameTag);
        }

        __try
        {
            const auto count = *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
            const auto base = *reinterpret_cast<volatile uintptr_t *>(a1 + k_compSlotCacheBasePtrOffset);
            if (base > 0x10000)
            {
                for (uint32_t e = 0; e < count; ++e)
                {
                    const auto entry = base + 24ULL * e;
                    const auto slotId = *reinterpret_cast<volatile uint16_t *>(entry);
                    for (std::size_t t = 0; t < clearTagCount; ++t)
                    {
                        if (slotId == clearTags[t])
                        {
                            *reinterpret_cast<volatile uint32_t *>(entry + 0x10) = 0;
                            break;
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("[dispatch] cache clear fault");
            suppress_vec().store(false, std::memory_order_release);
            return;
        }

        // Two-phase scene-graph tear-down before applying fakes.
        //
        // Phase A -- tear down the previous preset's fake meshes using
        //   lastIds[] as the itemId source.
        // Phase B -- tear down the REAL item in the auth table for every
        //   active slot whose new fake differs from the real.
        //
        // Both phases go through sub_14075FE60 which detaches particle emitters / anim controllers from the scene graph
        // via sub_1425EBAE0. Auth table is NOT mutated.
        //
        // Game slot tags verified via CE hardware BP on sub_148EB6700:
        //   Helm=0x03 Chest=0x04 Gloves=0x05 Boots=0x06 Cloak=0x10.
        // The TearDownSlot struct + k_tearDownSlots array are defined at file scope above so the dispatcher entry block
        // can also walk them when reading liveRealIds. k_tearDownCount is available from the same scope.

        // Snapshot the real equipped itemId for each slot up front so both phases and the PartShowSuppress mask can
        // compare without re-walking the auth table.
        std::uint16_t realItemId[k_tearDownCount]{};
        if (RealPartTearDown::is_ready())
        {
            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                realItemId[k] =
                    RealPartTearDown::get_real_item_id(reinterpret_cast<void *>(a1), k_tearDownSlots[k].gameTag);
            }

            // Phase A: previous fakes (from lastIds before this apply). A carrier/NPC fake entered the scene graph
            // under the char-class bypass, so tear_down_by_item_id must flip that byte back on to reach its auth row.
            // A fake the wearer can genuinely equip was applied WITHOUT the bypass (see should_skip_bypass), so its row
            // is reachable normally, and forcing the byte across its tear-down would restore the real item on the wrong
            // (default) body. Toggle the byte per slot so a mix of item kinds each restores correctly.
            const uintptr_t bypassAddr = resolved_addrs().charClassBypass;
            const auto &teardownOwner = current_apply_owner();

            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                const auto &td = k_tearDownSlots[k];
                const auto idx = static_cast<std::size_t>(td.slot);
                if (!slotNeedsWork[idx])
                    continue;
                const auto prevId = prevIds[idx];
                const auto prevCarrier = last_applied_carrier_ids()[idx];
                if (prevId == 0)
                {
                    // First-claim hide: active-none slot LT never owned (prevIds==0, no carrier history). Phase B alone
                    // calls the scene-graph tear-down once; empirically that's insufficient for slots where LT hasn't
                    // previously placed a carrier (e.g. Mask/Necklace when switching to an all-none preset on first
                    // apply). Doubling the call here matches the working manual path (transmog-something -> none),
                    // which fires Phase A on the prior carrier + Phase B on the real entry -- same hash, same slot tag,
                    // twice. No carrier history means the fake never entered under the bypass, so no toggle is needed.
                    const auto &m = mappings[idx];
                    if (m.active && m.targetItemId == 0 && liveRealIds[idx] != 0)
                    {
                        logger.trace("[dispatch] tear_down_fake slot={:#06x} "
                                     "itemId={:#06x} (first-claim hide)",
                                     td.gameTag, static_cast<std::uint16_t>(liveRealIds[idx]));
                        RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), liveRealIds[idx],
                                                               td.gameTag);
                    }
                    continue;
                }

                // Only a carrier fake the wearer could NOT equip needs the bypass flipped on to be reachable.
                const bool needBypass =
                    prevCarrier != 0 && !should_skip_bypass(static_cast<std::uint16_t>(prevId), teardownOwner);
                const bool bypassForSlot = needBypass && set_char_class_bypass(bypassAddr, 0xEB);
                if (bypassForSlot)
                    logger.trace("[carrier] charClass bypass ENABLED for slot={:#06x} tear-down", td.gameTag);

                // Phase A runs unconditionally: fake and real are treated equally, so even when the previous fake
                // matched the live real item we still tear it down.
                if (prevCarrier != 0 && prevCarrier != static_cast<std::uint16_t>(prevId))
                {
                    logger.trace("[dispatch] tear_down_fake slot={:#06x} "
                                 "carrier={:#06x} (then target={:#06x})",
                                 td.gameTag, prevCarrier, static_cast<std::uint16_t>(prevId));
                    RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), prevCarrier, td.gameTag);
                }
                RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), static_cast<std::uint16_t>(prevId),
                                                       td.gameTag);

                if (bypassForSlot)
                {
                    set_char_class_bypass(bypassAddr, 0x74);
                    logger.trace("[carrier] charClass bypass RESTORED for slot={:#06x} tear-down", td.gameTag);
                }
            }

            // Phase B: real items for any active slot. Runs unconditionally -- fake and real are treated equally, so
            // even when the new fake matches the live real we still tear down the real part. Only slots that had no
            // slotNeedsWork flag or are inactive skip.
            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                const auto &td = k_tearDownSlots[k];
                const auto idx = static_cast<std::size_t>(td.slot);
                if (!slotNeedsWork[idx])
                    continue;
                auto &m = mappings[idx];
                if (!m.active)
                    continue;
                if (RealPartTearDown::tear_down_real_part(reinterpret_cast<void *>(a1), td.gameTag))
                {
                    real_damaged()[idx] = true;
                }
            }
        }

        // Detect preset-switch AFTER tear-down completes but BEFORE the per-slot apply loop. If the new gear differs
        // from the gear active when swap was last applied, deactivate so the upcoming apply loop's substitutions don't
        // re-bind target wrappers to the new gear. (Reverse-write of prior tracked structs already happened
        // pre-tear-down so the engine could unlink them during tear-down.) Order: Helm/Chest/Cloak/ Gloves/Boots --
        // fixed 5-armor order matching the historical prefab-wrapper-swap notify_apply_starting contract.
        {
            const std::uint16_t newItems[5] = {
                static_cast<std::uint16_t>(mappings[static_cast<std::size_t>(TransmogSlot::Helm)].active
                                               ? mappings[static_cast<std::size_t>(TransmogSlot::Helm)].targetItemId
                                               : 0),
                static_cast<std::uint16_t>(mappings[static_cast<std::size_t>(TransmogSlot::Chest)].active
                                               ? mappings[static_cast<std::size_t>(TransmogSlot::Chest)].targetItemId
                                               : 0),
                static_cast<std::uint16_t>(mappings[static_cast<std::size_t>(TransmogSlot::Cloak)].active
                                               ? mappings[static_cast<std::size_t>(TransmogSlot::Cloak)].targetItemId
                                               : 0),
                static_cast<std::uint16_t>(mappings[static_cast<std::size_t>(TransmogSlot::Gloves)].active
                                               ? mappings[static_cast<std::size_t>(TransmogSlot::Gloves)].targetItemId
                                               : 0),
                static_cast<std::uint16_t>(mappings[static_cast<std::size_t>(TransmogSlot::Boots)].active
                                               ? mappings[static_cast<std::size_t>(TransmogSlot::Boots)].targetItemId
                                               : 0),
            };
            PrefabWrapperSwap::notify_apply_starting(newItems);
        }

        // Helper: look up a slot's real itemId from the snapshot taken during the tear-down phase.
        auto lookup_real_id = [&](std::size_t slotIdx) -> std::uint16_t
        {
            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                if (static_cast<std::size_t>(k_tearDownSlots[k].slot) == slotIdx)
                    return realItemId[k];
            }
            return 0;
        };

        uint32_t ourWrittenCount = 0;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            auto &m = mappings[i];
            if (!slotNeedsWork[i])
            {
                // Unchanged slot: preserve its lastIds entry but don't re-apply. If it has a live dispatch cache entry
                // the game keeps rendering it.
                if (m.active && m.targetItemId != 0)
                    lastIds[i] = m.targetItemId;
                continue;
            }
            if (!m.active || m.targetItemId == 0)
                continue;

            // Fake and real are treated equally: SlotPopulator runs unconditionally even when the new fake itemId
            // matches the intact live real item. The earlier skip-on-match fast-path was removed to keep the apply
            // sequence predictable; the incremental CPU savings did not justify the divergent code paths between
            // matched and unmatched re-applies.
            //
            // Decide: direct apply or carrier-assisted apply. current_apply_owner picks the editing character when the
            // dropdown pin is engaged, so a targeted-apply on a non-controlled body installs THAT body's carrier
            // family. See the matching block in apply_single_slot_-transmog for the cross-talk failure mode this
            // avoids.
            const auto tmSlot = static_cast<TransmogSlot>(i);
            const auto targetId = m.targetItemId;
            const auto &activeChar = current_apply_owner();
            const bool useCarrier = needs_carrier(targetId, activeChar);
            const uint16_t carrierId = useCarrier ? default_carrier_for_slot(tmSlot, activeChar) : 0;

            // Feed the active preset's per-slot dye state into the record injector so its inline detour on
            // sub_141E019E0 appends fabricated ARMOR_MOD records to the engine's publish vector post-trampoline. The
            // injection is independent of any real item the user wears.
            //
            // Bytes +7/+8/+9 of each record carry the literal RGB of the chosen shade. Channels with group_hash == 0
            // are inactive: the injector substitutes the first active channel's settings rather than skipping (sparse
            // injection lets the engine's natural records dominate and the dye does not render).
            const Preset *activePreset = PresetManager::instance().active_preset();
            const SlotDyeChannels *slotDye =
                (activePreset && i < activePreset->slots.size()) ? &activePreset->slots[i].dye : nullptr;
            if (slotDye && any_dye_active(*slotDye))
            {
                static_assert(Transmog::k_dyeChannelCount == DyeRecordInject::k_dyeChannelCount,
                              "channel-count mismatch between preset model "
                              "and dye injector");
                DyeRecordInject::ChannelState state[DyeRecordInject::k_dyeChannelCount];
                for (std::size_t k = 0; k < DyeRecordInject::k_dyeChannelCount; ++k)
                {
                    const auto &ch = (*slotDye)[k];
                    state[k] = {ch.group_hash, ch.r, ch.g, ch.b, ch.material_id, ch.repair_byte};
                }
                const bool sparse =
                    activePreset != nullptr && i < activePreset->slots.size() && activePreset->slots[i].dyeSparse;
                DyeRecordInject::set_slot_dye_state(state, sparse);
            }
            else
            {
                // Preset has no explicit dye -- let the engine's natural fake-item records flow. Mirrors the
                // apply_single_slot branch above.
                DyeRecordInject::clear_slot_dye_state();
            }

            ColorOverride::SetterSubstitute::set_active_slot(static_cast<int>(i));
            // See call site #1 (apply_single_slot) for the rationale. Pass user-intent (active ? user-chosen target :
            // 0) so untick doesn't trigger a target-change wipe of seeded placeholders.
            {
                auto &mapping = slot_mappings()[i];
                const std::uint32_t userIntent = mapping.active ? static_cast<std::uint32_t>(mapping.targetItemId) : 0u;
                ColorOverride::Reinit::notify_transmog_target(static_cast<int>(i), userIntent);
            }
            if (useCarrier && carrierId != 0)
            {
                logger.debug("Transmog APPLY (carrier): slot={}, "
                             "target={:#06x}, carrier={:#06x}",
                             slot_name(tmSlot), targetId, carrierId);
                logger.trace("[dispatch] applying slot={} targetId={:#06x} "
                             "via carrier={:#06x}",
                             i, targetId, carrierId);
                apply_transmog_with_carrier(a1, carrierId, targetId);
            }
            else
            {
                if (useCarrier)
                    logger.warning("Transmog APPLY: slot={} needs carrier "
                                   "but none resolved, falling back to direct",
                                   slot_name(tmSlot));
                logger.debug("Transmog APPLY: slot={}, target={:#06x}", slot_name(tmSlot), targetId);
                logger.trace("[dispatch] applying slot={} itemId={:#06x}", i, targetId);
                apply_transmog(a1, targetId);
            }

            DyeRecordInject::clear_slot_dye_state();
            uint32_t postCount = 0;
            __try
            {
                postCount = *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            logger.trace("[dispatch] post-apply slot={} liveCount={}", i, postCount);
            lastIds[i] = m.targetItemId;
            last_applied_carrier_ids()[i] = (useCarrier && carrierId != 0) ? carrierId : 0;
            ++ourWrittenCount;
        }

        // When a slot's checkbox is UNTICKED (!m.active), it was previously controlled by us (Phase B tore down the
        // real item). Restore the real item so it reappears. "Active + none" (checkbox ticked, dropdown=none) means
        // "show empty" -- do NOT restore.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const auto &m = mappings[i];
            if (!m.active && (prevIds[i] != 0 || real_damaged()[i]))
            {
                const std::uint16_t realId = lookup_real_id(i);
                if (realId != 0)
                {
                    logger.info("[dispatch] slot={} unticked -- "
                                "restoring real item {:#06x}",
                                slot_name(static_cast<TransmogSlot>(i)), realId);
                    // Snapshot the live dye records on this slot's auth-table entry and publish via the inject channel
                    // so apply_transmog repaints the restored real item in the user's actual inventory dye instead of
                    // the item's factory palette. Same pattern as Pass B in clear_all_transmog.
                    //
                    // We deliberately do NOT mirror this live dye into the active preset here. The `!m.active` gate
                    // above is shared with non-user-initiated mass-reset paths (mod-disable in this same function at
                    // the top, character switch / Unpin / preset load / ColorOverride re-init), so mirroring here would
                    // silently bake the real item's dye into the preset every time the mod is toggled off or the
                    // character is switched. Real-dye capture into the preset is explicit only: Capture Outfit (mass)
                    // or the per-slot "Sync from live" button in the dye popup.
                    const auto gameTag = game_slot_from_transmog(static_cast<TransmogSlot>(i));
                    if (!publish_entry_dye_for_gameslot(a1, gameTag))
                        DyeRecordInject::clear_slot_dye_state();
                    ColorOverride::SetterSubstitute::set_active_slot(static_cast<int>(i));
                    apply_transmog(a1, realId);
                    DyeRecordInject::clear_slot_dye_state();
                    ++ourWrittenCount;
                }
                else if (real_damaged()[i])
                {
                    // Real item was unequipped (realId=0) but we previously restored it via SlotPopulator, which left a
                    // scene-graph mesh entry. Tear it down so the visual actually disappears. The old real ID is still
                    // in last_applied_real_ids (not yet overwritten -- moved to end of function).
                    for (std::size_t k = 0; k < k_tearDownCount; ++k)
                    {
                        if (static_cast<std::size_t>(k_tearDownSlots[k].slot) != i)
                            continue;
                        // last_applied_real_ids is TransmogSlot-indexed, so look up by slot enum (== i), not by k.
                        const auto oldReal = last_applied_real_ids()[i];
                        if (oldReal != 0)
                        {
                            logger.info("[dispatch] slot={} unticked + "
                                        "unequipped -- tearing down restored "
                                        "mesh {:#06x}",
                                        slot_name(static_cast<TransmogSlot>(i)), oldReal);
                            RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), oldReal,
                                                                   k_tearDownSlots[k].gameTag);
                        }
                        break;
                    }
                }
                // Clear damage flag so this slot is fully released back to the game. Without this, future apply cycles
                // keep treating it as managed.
                real_damaged()[i] = false;
            }
            if (!m.active || m.targetItemId == 0)
                last_applied_carrier_ids()[i] = 0;
        }

        // Cleanup pass: tear down stale scene-graph meshes left by a prior restore via SlotPopulator. This handles the
        // case where apply_single_slot restored the real item (creating a scene graph entry), then the user unequipped
        // in the game inventory. The untick-restore block above doesn't catch this because apply_single_slot already
        // cleared prevIds and real_damaged. We detect it here via slotNeedsWork + unticked + real=0 + old real in
        // last_applied_real_ids (not yet overwritten).
        for (std::size_t k = 0; k < k_tearDownCount; ++k)
        {
            const auto &td = k_tearDownSlots[k];
            const auto idx = static_cast<std::size_t>(td.slot);
            if (!slotNeedsWork[idx])
                continue;
            if (mappings[idx].active)
                continue;
            if (realItemId[k] != 0)
                continue;
            // last_applied_real_ids is TransmogSlot-indexed -- look up by `idx` (the slot enum), not the iteration
            // counter.
            const auto oldReal = last_applied_real_ids()[idx];
            if (oldReal == 0)
                continue;
            logger.info("[dispatch] slot={} cleanup -- tearing down "
                        "stale restore mesh {:#06x}",
                        slot_name(td.slot), oldReal);
            RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), oldReal, td.gameTag);
        }

        // Count was NOT zeroed -- unchanged slots' entries are still live with their original subCount. Log the final
        // state for diagnostics.
        __try
        {
            uint32_t liveCount = *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
            logger.trace("[dispatch] post-apply liveCount={} "
                         "ourWrittenCount={}",
                         liveCount, ourWrittenCount);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        // targetMask: slots with a fake mesh to render. activeMask: slots the user explicitly controls.
        std::uint32_t targetMask = 0;
        std::uint32_t activeMask = 0;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (lastIds[i] != 0)
                targetMask |= (std::uint32_t{1} << i);
            if (mappings[i].active)
                activeMask |= (std::uint32_t{1} << i);
        }

        // Suppression rule: suppress every active slot whose real item has been (or will be) torn down. If the fake
        // itemId matches the real equipped id, the real item is still live and we must NOT suppress it.
        std::uint32_t suppressMask = 0;
        for (std::size_t k = 0; k < k_tearDownCount; ++k)
        {
            const auto idx = static_cast<std::size_t>(k_tearDownSlots[k].slot);
            const auto &m = mappings[idx];
            if (!m.active)
                continue;
            if (m.targetItemId != 0 && static_cast<std::uint16_t>(m.targetItemId) == realItemId[k])
                continue;
            suppressMask |= (std::uint32_t{1} << idx);
        }

        {
            // Trivially-destructible char buffers -- std::string here would violate SEH/object-unwinding (the function
            // contains __try frames). Split prev / now across two log lines so each fits in a normal terminal width.
            char prevBuf[256];
            char nowBuf[256];
            std::size_t pOff = 0;
            std::size_t nOff = 0;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                const int np = std::snprintf(prevBuf + pOff, sizeof(prevBuf) - pOff, "%s0x%04x", i ? "," : "",
                                             static_cast<unsigned>(prevIds[i]));
                if (np > 0)
                    pOff += static_cast<std::size_t>(np);
                const int nn = std::snprintf(nowBuf + nOff, sizeof(nowBuf) - nOff, "%s0x%04x", i ? "," : "",
                                             static_cast<unsigned>(lastIds[i]));
                if (nn > 0)
                    nOff += static_cast<std::size_t>(nn);
            }
            logger.info("apply_all_transmog prev=[{}]", prevBuf);
            logger.info("apply_all_transmog now=[{}] target={:#x} "
                        "active={:#x} suppress={:#x}",
                        nowBuf, targetMask, activeMask, suppressMask);
        }

        PartShowSuppress::set_mask(static_cast<uint32_t>(suppressMask));

        // Commit the real-armor snapshot AFTER all applies succeed. If the function crashed (SEH during reload), this
        // line is never reached and the next retry correctly detects the real-armor change.
        last_applied_real_ids() = liveRealIds;

        // Record this apply's itemIds with body-mesh pointer swap so the next apply can detect a preset-switch and
        // auto-deactivate.
        {
            const std::uint16_t appliedItems[5] = {
                static_cast<std::uint16_t>(lastIds[static_cast<std::size_t>(TransmogSlot::Helm)]),
                static_cast<std::uint16_t>(lastIds[static_cast<std::size_t>(TransmogSlot::Chest)]),
                static_cast<std::uint16_t>(lastIds[static_cast<std::size_t>(TransmogSlot::Cloak)]),
                static_cast<std::uint16_t>(lastIds[static_cast<std::size_t>(TransmogSlot::Gloves)]),
                static_cast<std::uint16_t>(lastIds[static_cast<std::size_t>(TransmogSlot::Boots)]),
            };
            PrefabWrapperSwap::notify_apply_finished(appliedItems);
        }

        suppress_vec().store(false, std::memory_order_release);

        // PrefabWrapperSwap stays active across applies. Auto-deactivate after each apply is intentionally not
        // scheduled:
        // the wrapper-substitution path has no cheap teardown (an earlier heap-walk-on-deactivate attempt stalled
        // preset switches by ~1 minute) and the residual helm leak sits in scene-graph children re-parented via
        // sub_1425F41F0's runtime-resource-pointer keying, outside the wrapper-substitution path entirely. Clearing
        // that residue requires PAZ-level patching; users invoke LT's Clear button when they want the swap explicitly
        // torn down.
    }

    void clear_all_transmog(__int64 a1)
    {
        auto &logger = DMK::Logger::get_instance();
        auto &lastIds = last_applied_ids();

        // Fallback only -- see apply_single_slot_transmog comment.
        if (a1 < 0x10000 && world_system_ptr().load(std::memory_order_acquire))
        {
            auto fresh = resolve_player_component();
            if (fresh > 0x10000)
                a1 = fresh;
        }

        suppress_vec().store(true, std::memory_order_release);

        // Snapshot the fakes we previously applied BEFORE clearing lastIds. Iteration order across `k_slotMetadata` is
        // irrelevant for correctness: prevFakeId / prevCarrierId are indexed by `k` (the array slot), and the per-slot
        // snapshot reads `lastIds` by `slot` (the TransmogSlot enum value). Engine-only tags 0x000E and 0x0015 are
        // absent from `k_slotMetadata` by design (see `TransmogSlot` enum in `shared_state.hpp`), so the loop skips
        // them automatically.
        std::uint16_t prevFakeId[k_slotCount]{};
        for (std::size_t k = 0; k < k_slotCount; ++k)
        {
            const auto idx = static_cast<std::size_t>(k_slotMetadata[k].slot);
            prevFakeId[k] = static_cast<std::uint16_t>(lastIds[idx]);
        }

        // Snapshot carrier IDs before clearing.
        std::uint16_t prevCarrierId[k_slotCount]{};
        for (std::size_t k = 0; k < k_slotCount; ++k)
        {
            const auto idx = static_cast<std::size_t>(k_slotMetadata[k].slot);
            prevCarrierId[k] = last_applied_carrier_ids()[idx];
        }

        // Clear lastIds, carrier IDs, and per-slot damage flags.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            lastIds[i] = 0;
            last_applied_carrier_ids()[i] = 0;
            real_damaged()[i] = false;
        }

        // Pass A: tear down orphan fakes.
        if (RealPartTearDown::is_ready())
        {
            // A carrier/NPC fake entered the scene graph under the char-class bypass, so tear_down_by_item_id must flip
            // that byte back on to reach its auth row -- otherwise the tear-down silently fails and the carrier visual
            // persists (the failure mode that surfaces when toggling LT off or hitting Clear on such a slot). A fake
            // the wearer can genuinely equip was applied WITHOUT the bypass (see should_skip_bypass), so its row is
            // reachable normally, and forcing the byte across its tear-down would restore the real item on the wrong
            // (default) body. Toggle the byte per slot so a mix of item kinds each restores correctly. Mirrors the
            // Phase A toggle in apply_all_transmog; set_char_class_bypass is null-safe (false when the address is 0).
            const uintptr_t bypassAddr = resolved_addrs().charClassBypass;
            const auto &teardownOwner = current_apply_owner();

            for (std::size_t k = 0; k < k_slotCount; ++k)
            {
                const auto gameTag = static_cast<std::uint16_t>(k_slotMetadata[k].gameTag);
                const auto fakeId = prevFakeId[k];
                const auto cId = prevCarrierId[k];
                if (fakeId == 0)
                    continue;
                const auto realId = RealPartTearDown::get_real_item_id(reinterpret_cast<void *>(a1), gameTag);
                if (realId == fakeId && cId == 0)
                {
                    logger.trace("[clear] orphan-check slot={:#06x} fake={:#06x} "
                                 "skipped (matches real, no carrier)",
                                 gameTag, fakeId);
                    continue;
                }

                // Only a carrier fake the wearer could NOT equip needs the bypass flipped on to be reachable.
                const bool needBypass = cId != 0 && !should_skip_bypass(fakeId, teardownOwner);
                const bool bypassForSlot = needBypass && set_char_class_bypass(bypassAddr, 0xEB);
                if (bypassForSlot)
                    logger.trace("[clear] charClass bypass ENABLED for slot={:#06x} tear-down", gameTag);

                // Tear down carrier identity first if used.
                if (cId != 0 && cId != fakeId)
                {
                    logger.info("[clear] tearing carrier slot={:#06x} "
                                "carrier={:#06x} (real={:#06x})",
                                gameTag, cId, realId);
                    RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), cId, gameTag);
                }
                logger.info("[clear] tearing orphan fake slot={:#06x} itemId={:#06x} "
                            "(real={:#06x} carrier={:#06x})",
                            gameTag, fakeId, realId, cId);
                RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), fakeId, gameTag);

                if (bypassForSlot)
                {
                    set_char_class_bypass(bypassAddr, 0x74);
                    logger.trace("[clear] charClass bypass RESTORED for slot={:#06x} tear-down", gameTag);
                }
            }
        }
        else
        {
            logger.debug("[clear] RealPartTearDown not ready -- pass A skipped");
        }

        // Reset the real-item snapshot so the next apply re-reads from the live auth table.
        last_applied_real_ids().fill(0);

        __try
        {
            auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + k_compEntryTablePtrOffset);
            if (entryDesc > 0x10000)
            {
                auto entryArray = *reinterpret_cast<uintptr_t *>(entryDesc + 8);
                auto entryCount = *reinterpret_cast<uint32_t *>(entryDesc + 16);

                auto savedCount = *reinterpret_cast<uint32_t *>(a1 + k_compSlotCacheCountOffset);
                *reinterpret_cast<uint32_t *>(a1 + k_compSlotCacheCountOffset) = 0;

                for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
                {
                    auto base = entryArray + e * k_compEntryStride;
                    auto gameSlot = *reinterpret_cast<int16_t *>(base + k_compEntrySlotTagOffset);
                    auto itemId = *reinterpret_cast<uint16_t *>(base + k_compEntryItemIdOffset);

                    if (itemId == 0 || itemId == 0xFFFF)
                        continue;

                    auto tmSlot = slot_from_game_slot(gameSlot);
                    if (!tmSlot.has_value())
                        continue;

                    // Snapshot the equipped item's live dye records and re-publish via the inject channel so the
                    // following apply_transmog -> SlotPopulator ->
                    // DyeCopier round-trip emits them into the render struct's dst+120. Without this, the synthesized
                    // swapEntry passes through DyeCopier empty and the engine resolves the slot to its factory palette,
                    // painting toggled-off items un-dyed.
                    DyeRecordInject::ChannelState liveDye[DyeRecordInject::k_dyeChannelCount];
                    if (DyeRecordInject::read_entry_dye_records(base, liveDye) > 0)
                    {
                        DyeRecordInject::log_dye_snapshot("restore", slot_name(*tmSlot), liveDye);
                        // sparse: mirror exactly the source channels so we don't paint mesh parts the real item never
                        // colored.
                        DyeRecordInject::set_slot_dye_state(liveDye, /*sparse=*/true);
                    }
                    else
                    {
                        DyeRecordInject::clear_slot_dye_state();
                    }

                    logger.debug("Transmog RESTORE: real item {:#06x} for slot {}", itemId, game_slot_name(gameSlot));
                    apply_transmog(a1, itemId);

                    DyeRecordInject::clear_slot_dye_state();
                }

                // Restore count to the larger of saved and live.
                uint32_t liveCount = *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
                uint32_t finalCount = (liveCount > savedCount) ? liveCount : savedCount;
                *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset) = finalCount;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("Transmog clear exception during restore");
        }

        PartShowSuppress::clear_all_suppressed();

        // Intentionally do NOT call PrefabWrapperSwap::deactivate_for_clear() here.
        //
        // The natpipe hook on sub_142711DF0 must stay armed (s_active=true with s_swapMapPerChar populated) so any
        // later organic unequip / scene-graph teardown -- triggered when the user swaps gear via the radial after a
        // Clear -- can still find and unlink the Bastier-target wrappers LT installed in parent+88. Disarming it here
        // makes the engine search with Kliff src wrappers, miss the Bastier targets, and leak ghost meshes (the
        // ghost-helm leak this hook was originally added to fix).
        //
        // The on_struct_copy hook is independently silenced after toggle-off by its in_transmog() gate, so leaving the
        // swap map armed only matters during the engine's own cleanup walks, which is exactly when it must fire.

        logger.info("Transmog CLEAR: done");
        suppress_vec().store(false, std::memory_order_release);
    }

} // namespace Transmog
