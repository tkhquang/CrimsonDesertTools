#include "transmog_apply.hpp"
#include "item_name_table.hpp"
#include "part_show_suppress.hpp"
#include "preset_manager.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "transmog_map.hpp"
#include "transmog_worker.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>

namespace Transmog
{
    // SlotPopulator (sub_14076C960) maintains a dispatch cache on the
    // component at (basePtr, count, cap). The triple shifted 0x20
    // bytes higher between v1.03.01 and v1.04.00 as the component
    // gained new fields lower in the struct:
    //
    //   v1.03.01 -- basePtr @ +0x1B8, count @ +0x1C0, cap @ +0x1C4
    //   v1.04.00 -- basePtr @ +0x1D8, count @ +0x1E0, cap @ +0x1E4
    //
    // Reading the old offsets on v1.04.00 returns zero for count (the
    // u32 that lives there is always 0), which makes apply look like
    // a no-op; writing the old offsets scribbles into adjacent fields
    // and corrupts the component a slot at a time.
    constexpr std::ptrdiff_t k_compSlotCacheBasePtrOffset = 0x1D8;
    constexpr std::ptrdiff_t k_compSlotCacheCountOffset   = 0x1E0;
    constexpr std::ptrdiff_t k_compSlotCacheCapOffset     = 0x1E4;

    // Pointer to the PartDef/auth-table container on a1.
    //
    //   v1.03.01 -- container @ a1 + 0x78
    //   v1.04.00 -- container @ a1 + 0x88   (+0x10 of new fields below)
    //   v1.05.00 -- container @ a1 + 0x88   (unchanged from v1.04)
    //
    // Mirrors k_containerPtrOffset in real_part_tear_down.cpp. Reading
    // the old offset on v1.04.00 yields a non-container qword that
    // either dereferences to junk or fails the >0x10000 sanity gate,
    // skipping the real-item restore loop entirely.
    constexpr std::ptrdiff_t k_compEntryTablePtrOffset    = 0x88;

    // Entry layout within the auth-table array. v1.05 grew the entry
    // by 8 bytes and shifted the slot tag accordingly:
    //
    //   v1.04.00: stride=0xC8 (200), slotTag@+0xC0
    //   v1.05.00: stride=0xD0 (208), slotTag@+0xC8
    //
    // primary item id at +0x08 is unchanged. Slot-tag VALUES themselves
    // are unchanged from v1.04; only the position within the entry
    // shifted. Mirrors k_entryStride / k_entrySlotTagOffset in
    // real_part_tear_down.cpp.
    constexpr std::ptrdiff_t k_compEntryStride           = 0xD0;
    constexpr std::ptrdiff_t k_compEntryItemIdOffset     = 0x08;
    constexpr std::ptrdiff_t k_compEntrySlotTagOffset    = 0xC8;

    void apply_transmog(__int64 a1, uint16_t targetId)
    {
        auto slotPop = slot_populator_fn();
        auto initEntry = init_swap_entry_fn();
        if (!slotPop || !initEntry)
            return;

        // Build 16-byte item data structure for SlotPopulator.
        alignas(16) uint8_t itemData[16]{};
        *reinterpret_cast<uint16_t *>(itemData + 0) = targetId;
        itemData[2] = 2;
        *reinterpret_cast<uint32_t *>(itemData + 4) = 0xFFFFFFFF;
        *reinterpret_cast<uint16_t *>(itemData + 12) = 0xFFFF;

        // Build empty swap entry.
        alignas(16) uint8_t swapEntry[256]{};
        initEntry(reinterpret_cast<__int64>(swapEntry));

        in_transmog().store(true, std::memory_order_relaxed);
        slotPop(a1, reinterpret_cast<unsigned __int16 *>(itemData),
                reinterpret_cast<__int64>(swapEntry));
        in_transmog().store(false, std::memory_order_relaxed);
    }

    // ── Default carrier set (per character) ─────────────────────────────
    // Each entry must be a valid item for THAT character in the given
    // slot -- something the engine's equip class-gate accepts. Kliff and
    // Oongka share Kliff_PlateArmor_* (Oongka's equip-type matches
    // Kliff's). Damiane has her own armor namespace (Demian_*) which
    // Kliff cannot wear, but the engine accepts them on Damiane even
    // though they are marked PlayerSafe=no in the item catalog. The
    // PlayerSafe flag reflects Kliff compatibility only.
    //
    // If a name fails to resolve the slot falls back to direct equip
    // (which may silently fail for NPC/variant items).

    static constexpr const char *k_kliffCarriers[] = {
        "Kliff_PlateArmor_Helm",   // TransmogSlot::Helm
        "Kliff_PlateArmor_Armor",  // TransmogSlot::Chest
        "Kliff_PlateArmor_Cloak",  // TransmogSlot::Cloak
        "Kliff_PlateArmor_Gloves", // TransmogSlot::Gloves
        "Kliff_Plate_Boots",       // TransmogSlot::Boots
    };

    static constexpr const char *k_damianeCarriers[] = {
        "Demian_PlateArmor_Helm_I",   // TransmogSlot::Helm
        "Demian_Leather_Armor",       // TransmogSlot::Chest
        "Demian_Leather_Cloak",       // TransmogSlot::Cloak
        "Demian_Leather_Gloves_I",    // TransmogSlot::Gloves
        "Demian_Plate_Boots_III",     // TransmogSlot::Boots
    };

    // Oongka carriers. Despite sharing 3/4 male-body classifier
    // tokens with Kliff, the engine rejects Kliff-equip-type (0x0004)
    // items at Oongka's equip gate -- Oongka's slot class is 0x0001.
    // Native Oongka_* items pass the gate cleanly; the hybrid then
    // patches their +0x42 over a Kliff target so Oongka can wear
    // cross-character armor visuals.
    static constexpr const char *k_oongkaCarriers[] = {
        "Oongka_PlateArmor_Helm_II",   // TransmogSlot::Helm
        "Oongka_Basic_Leather_Armor",  // TransmogSlot::Chest
        "Oongka_Basic_Leather_Cloak",  // TransmogSlot::Cloak
        "Oongka_Basic_Leather_Gloves", // TransmogSlot::Gloves
        "Oongka_PlateArmor_Boots_II",  // TransmogSlot::Boots
    };

    uint16_t default_carrier_for_slot(
        TransmogSlot slot, const std::string &charName)
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= k_slotCount)
            return 0;
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
            return 0;

        const char *name = k_kliffCarriers[idx];
        if (charName == "Damiane")
            name = k_damianeCarriers[idx];
        else if (charName == "Oongka")
            name = k_oongkaCarriers[idx];

        auto id = table.id_of(name);
        if (id.has_value())
            return *id;

        // Fallback: if a character-specific carrier name did not
        // resolve (missing from catalog, renamed), try Kliff's set.
        if (name != k_kliffCarriers[idx])
        {
            auto kliff = table.id_of(k_kliffCarriers[idx]);
            return kliff.value_or(0);
        }
        return 0;
    }

    // Expected equip-type u16 (at desc+0x42) for the given playable
    // character. CE-verified 2026-04-21:
    //   Kliff   = 0x0004
    //   Oongka  = 0x0001
    //   Damiane = 0x0001
    // Unknown characters default to Kliff's value so legacy call
    // paths keep their prior behaviour.
    static std::uint16_t expected_equip_type_for_char(
        const std::string &charName) noexcept
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

        // Variant-meta items always need the carrier-patch path so
        // the NPC variant descriptor gets swapped in for visuals
        // while a clean carrier supplies the rule list.
        if (table.has_variant_meta(itemId))
            return true;

        // Equip-type mismatch: the item's +0x42 slot class doesn't
        // match what the active character's inventory slot accepts.
        // Without the carrier path the engine rejects the item at
        // the equip gate (Oongka picking Kliff's 0x0004 armor, Kliff
        // picking Damiane's 0x0001 armor, etc.). The hybrid patches
        // the carrier's equip-type over the target descriptor so
        // the slot accepts while the target's visuals render.
        const std::uint16_t expected =
            expected_equip_type_for_char(charName);
        const std::uint16_t actual = table.equip_type_of(itemId);
        // equip_type_of returns 0 if the catalog hasn't resolved this
        // id yet; fall back to the old broad "is_player_compatible"
        // gate in that case so we don't silently direct-apply a
        // cross-class item during the deferred-scan window.
        if (actual == 0)
            return !table.is_player_compatible(itemId) ||
                   table.has_npc_equip_type(itemId);

        return actual != expected;
    }

    // Legacy wrapper retained for any call sites that still assume
    // Kliff. Matches the pre-2026-04-21 behaviour exactly.
    bool needs_carrier(uint16_t itemId)
    {
        return needs_carrier(itemId, std::string("Kliff"));
    }

    // SEH-safe memory helpers (MSVC: __try cannot coexist with C++
    // object unwinding in the same function).
    static uintptr_t read_qword_seh(uintptr_t addr) noexcept
    {
        __try
        {
            return *reinterpret_cast<volatile uintptr_t *>(addr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
    static bool memcpy_seh(void *dst, const void *src, size_t n) noexcept
    {
        __try
        {
            std::memcpy(dst, src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Descriptor size. Stride between consecutive descriptors observed
    // in CE on the live build:
    //
    //   v1.03.01 -- 0x400 (1024)
    //   v1.04.00 -- 0xA00 (2560)
    //
    // The hybrid buffer is memcpy'd from the target descriptor up to
    // this size; any byte beyond ends up as VirtualAlloc-zero. If
    // SlotPopulator or its callees read past the copied range they see
    // zeros instead of the real descriptor data, which faults when the
    // zero is treated as an embedded pointer, vtable, or index. Keep
    // this at or above the live stride; over-copy is safe because the
    // source descriptor is always at least a full stride wide in the
    // pool allocator.
    static constexpr std::size_t k_descBufSize = 0xA00;

    // Descriptor offsets read by SlotPopulator for character-context
    // matching BEFORE visual/mesh data. Must come from the CARRIER so
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
    // the read-after-write can see stale values when page protection
    // is restored between enable/restore cycles.
    static bool set_char_class_bypass(uintptr_t addr, uint8_t val) noexcept
    {
        if (!addr)
            return false;
        const auto byteVal = static_cast<std::byte>(val);
        return DMK::Memory::write_bytes(
                   reinterpret_cast<std::byte *>(addr), &byteVal, 1)
            .has_value();
    }

    // Swaps descriptor pointer, toggles char-class bypass, calls
    // SlotPopulator, then unconditionally restores both via __finally.
    //
    // __finally (not __except): runs cleanup WITHOUT catching the
    // exception. Game-internal SEH exceptions (lazy loading, page
    // faults) continue propagating to the game's own handlers.
    // __except would intercept them and break the game.
    //
    // This is critical because apply_transmog faults on early load
    // attempts (game data not ready). Without __finally, the
    // descriptor pointer and bypass byte would be left corrupted.
    static void carrier_swap_and_call(
        __int64 a1, uint16_t carrierId,
        uintptr_t carrierSlotAddr, uintptr_t hybridAddr,
        uintptr_t bypassAddr) noexcept
    {
        const auto savedDesc = static_cast<LONG64>(
            InterlockedExchange64(
                reinterpret_cast<volatile LONG64 *>(carrierSlotAddr),
                static_cast<LONG64>(hybridAddr)));

        const bool bypassApplied =
            set_char_class_bypass(bypassAddr, 0xEB);

        __try
        {
            apply_transmog(a1, carrierId);
        }
        __finally
        {
            if (bypassApplied)
                set_char_class_bypass(bypassAddr, 0x74);

            InterlockedExchange64(
                reinterpret_cast<volatile LONG64 *>(carrierSlotAddr),
                savedDesc);
        }
    }

    void apply_transmog_with_carrier(
        __int64 a1, uint16_t carrierId, uint16_t targetId)
    {
        auto &logger = DMK::Logger::get_instance();

        if (carrierId == 0 || carrierId == targetId)
        {
            logger.trace("[carrier] carrierId={:#06x} == targetId={:#06x}, "
                         "direct apply",
                         carrierId, targetId);
            apply_transmog(a1, targetId);
            return;
        }

        const auto &table = ItemNameTable::instance();
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

        const auto carrierSlotAddr =
            ci.ptrArray + static_cast<uint64_t>(carrierId) * 8;
        const uintptr_t targetDesc = read_qword_seh(
            ci.ptrArray + static_cast<uint64_t>(targetId) * 8);
        const uintptr_t carrierDesc = read_qword_seh(carrierSlotAddr);

        if (targetDesc < 0x10000 || carrierDesc < 0x10000)
        {
            logger.warning("[carrier] bad descriptor: "
                           "carrier={:#06x}={:#018x} target={:#06x}={:#018x}",
                           carrierId, carrierDesc, targetId, targetDesc);
            apply_transmog(a1, targetId);
            return;
        }

        // ── Build hybrid descriptor on the heap ─────────────────────────
        // Must use VirtualAlloc so the address is in the high heap range
        // (game may reject low stack addresses in pointer sanity checks).
        // Base = target's visual data. Overlay = carrier's matching fields.
        auto *hybrid = static_cast<uint8_t *>(
            VirtualAlloc(nullptr, k_descBufSize,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!hybrid)
        {
            logger.warning("[carrier] VirtualAlloc failed for hybrid buffer");
            apply_transmog(a1, targetId);
            return;
        }

        if (!memcpy_seh(hybrid, reinterpret_cast<const void *>(targetDesc),
                        k_descBufSize))
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
            if (!memcpy_seh(hybrid + p.off,
                            reinterpret_cast<const void *>(carrierDesc + p.off),
                            p.len))
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
        const uintptr_t bypassAddr = resolved_addrs().charClassBypass;

        logger.trace("[carrier] HYBRID built: {} bytes patched, "
                     "carrier={:#06x} desc={:#018x}, "
                     "target={:#06x} desc={:#018x}, hybrid={:#018x}",
                     patchedBytes,
                     carrierId, carrierDesc, targetId, targetDesc,
                     hybridAddr);

        // SEH-isolated: swap descriptor, toggle bypass, call
        // SlotPopulator, then unconditionally restore both.
        carrier_swap_and_call(
            a1, carrierId, carrierSlotAddr, hybridAddr, bypassAddr);

        logger.trace("[carrier] POST: carrier={:#06x} target={:#06x}",
                     carrierId, targetId);

        VirtualFree(hybrid, 0, MEM_RELEASE);
    }

    // ── Single-slot apply ──────────────────────────────────────────────
    //
    // Scoped version of apply_all_transmog that only touches ONE slot.
    // Used by hover-preview to avoid the full tear-down + re-apply of
    // all 5 slots, which causes visible flicker on unchanged gear.
    //
    // Differences from apply_all_transmog:
    //   - Dispatch cache: only clears entries matching this slot's game
    //     tag (by walking the 24-byte stride array). Does NOT reset the
    //     global count to zero -- other slots' entries stay valid.
    //   - Tear-down: Phase A (previous fake) and Phase B (real item)
    //     scoped to one slot.
    //   - Apply: single call to apply_transmog or
    //     apply_transmog_with_carrier.
    //   - State: only updates lastIds, carrier, suppress for this slot.

    // Game slot tags per TransmogSlot index. Must stay in sync with
    // k_tearDownSlots inside apply_all_transmog.
    static constexpr std::uint16_t k_gameSlotTags[k_slotCount] = {
        0x0003, // Helm
        0x0004, // Chest
        0x0010, // Cloak
        0x0005, // Gloves
        0x0006, // Boots
    };

    void apply_single_slot_transmog(__int64 a1, std::size_t slotIdx)
    {
        if (slotIdx >= k_slotCount)
            return;

        auto &logger = DMK::Logger::get_instance();
        auto &mappings = slot_mappings();
        auto &lastIds = last_applied_ids();
        auto &m = mappings[slotIdx];

        // resolve_player_component() walks WorldSystem → ActorManager
        // → UserActor → actor, which we confirmed in 2026-04-21 CE
        // probes always returns Kliff's component regardless of the
        // currently-controlled character. Using it unconditionally
        // clobbered per-character a1 values from VEC/BatchEquip hooks.
        // Keep it ONLY as a fallback when the passed-in a1 is invalid.
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
        const uint16_t gameTag = k_gameSlotTags[slotIdx];

        // Compute target: active slot with non-zero id → transmog,
        // otherwise clear this slot.
        const uint16_t targetId =
            (m.active && m.targetItemId != 0) ? m.targetItemId : 0;

        // Early-out: nothing changed for this slot.
        if (targetId == prevId && targetId != 0)
        {
            logger.trace("apply_single_slot: slot={} id={:#06x} unchanged",
                         slotIdx, targetId);
            suppress_vec().store(false, std::memory_order_release);
            return;
        }

        // --- Scoped dispatch cache clear ---
        // Walk the 24-byte stride cache and zero subCount only for
        // entries whose slotNativeId matches our game tag. This leaves
        // other slots' blobs untouched so VEC doesn't re-dispatch them.
        __try
        {
            const auto count =
                *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
            const auto base =
                *reinterpret_cast<volatile uintptr_t *>(a1 + k_compSlotCacheBasePtrOffset);
            if (base > 0x10000)
            {
                for (uint32_t e = 0; e < count; ++e)
                {
                    const auto entry = base + 24ULL * e;
                    const auto slotId =
                        *reinterpret_cast<volatile uint16_t *>(entry);
                    if (slotId == gameTag)
                        *reinterpret_cast<volatile uint32_t *>(
                            entry + 0x10) = 0;
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
            realId = RealPartTearDown::get_real_item_id(
                reinterpret_cast<void *>(a1), gameTag);

            // Phase A: tear down previous fake. Runs even when the
            // previous fake itemId matches the live real item; we
            // treat fake and real the same so the tear-down/apply
            // sequence is always complete.
            if (prevId != 0)
            {
                const auto prevCarrier = last_applied_carrier_ids()[slotIdx];
                const uintptr_t bypassAddr =
                    resolved_addrs().charClassBypass;
                bool bypassApplied = false;
                if (prevCarrier != 0)
                    bypassApplied =
                        set_char_class_bypass(bypassAddr, 0xEB);

                if (prevCarrier != 0 &&
                    prevCarrier != static_cast<uint16_t>(prevId))
                {
                    RealPartTearDown::tear_down_by_item_id(
                        reinterpret_cast<void *>(a1), prevCarrier, gameTag);
                }
                RealPartTearDown::tear_down_by_item_id(
                    reinterpret_cast<void *>(a1),
                    static_cast<uint16_t>(prevId), gameTag);

                if (bypassApplied)
                    set_char_class_bypass(bypassAddr, 0x74);
            }

            // Phase B: tear down the real item. Runs unconditionally
            // (fake == real is treated the same as fake != real).
            if (targetId != 0)
            {
                if (RealPartTearDown::tear_down_real_part(
                        reinterpret_cast<void *>(a1), gameTag))
                    real_damaged()[slotIdx] = true;
            }
        }

        // --- Apply ---
        if (targetId != 0)
        {
            const auto tmSlot = static_cast<TransmogSlot>(slotIdx);
            const auto &activeChar =
                PresetManager::instance().active_character();
            const bool useCarrier = needs_carrier(targetId, activeChar);
            const uint16_t carrierId =
                useCarrier
                    ? default_carrier_for_slot(tmSlot, activeChar)
                    : 0;

            if (useCarrier && carrierId != 0)
            {
                logger.debug("apply_single_slot: slot={} target={:#06x} "
                             "carrier={:#06x}",
                             slot_name(tmSlot), targetId, carrierId);
                apply_transmog_with_carrier(a1, carrierId, targetId);
            }
            else
            {
                logger.debug("apply_single_slot: slot={} target={:#06x}",
                             slot_name(tmSlot), targetId);
                apply_transmog(a1, targetId);
            }

            lastIds[slotIdx] = targetId;
            last_applied_carrier_ids()[slotIdx] =
                (useCarrier && carrierId != 0) ? carrierId : 0;
            // Phase B set real_damaged when tearing down the real item
            // for this slot. Now that the fake is successfully applied,
            // clear it so apply_all_transmog doesn't see stale damage
            // state for this slot on subsequent cycles.
            real_damaged()[slotIdx] = false;
        }
        else
        {
            // Clearing this slot. Two cases:
            //  - active + none (checkbox ticked, picker = none): user
            //    wants to show an EMPTY slot (bare head, etc.). Do NOT
            //    restore the real item -- Phase B already tore it down.
            //  - inactive (!m.active): slot was previously controlled
            //    by us; restore the real item so it reappears.
            const bool showEmpty = m.active;
            if (!showEmpty && (prevId != 0 || real_damaged()[slotIdx]))
            {
                if (realId != 0)
                {
                    logger.debug("apply_single_slot: slot={} restoring "
                                 "real {:#06x}",
                                 slot_name(static_cast<TransmogSlot>(
                                     slotIdx)),
                                 realId);
                    apply_transmog(a1, realId);
                }
            }
            lastIds[slotIdx] = 0;
            last_applied_carrier_ids()[slotIdx] = 0;
            // Clear damage flag so the slot is fully released back to
            // the game. Without this, apply_all_transmog's untick-restore
            // and slotNeedsWork checks see stale damage state and keep
            // interfering with an unmanaged slot.
            real_damaged()[slotIdx] = false;
        }

        // Update suppress mask for this slot only. Rebuild full mask
        // from current state rather than toggling one bit, to stay
        // consistent with apply_all_transmog's mask logic.
        std::uint32_t suppressMask = 0;
        for (std::size_t k = 0; k < k_slotCount; ++k)
        {
            const auto &sm = mappings[k];
            if (!sm.active)
                continue;
            const std::uint16_t slotReal =
                RealPartTearDown::is_ready()
                    ? RealPartTearDown::get_real_item_id(
                          reinterpret_cast<void *>(a1), k_gameSlotTags[k])
                    : 0;
            if (sm.targetItemId != 0 &&
                static_cast<uint16_t>(sm.targetItemId) == slotReal)
                continue;
            suppressMask |= (std::uint32_t{1} << k);
        }
        PartShowSuppress::set_mask(suppressMask);

        logger.trace("apply_single_slot: slot={} done, suppress={:#x}",
                     slotIdx, suppressMask);

        suppress_vec().store(false, std::memory_order_release);
    }

    void apply_all_transmog(__int64 a1)
    {
        auto &logger = DMK::Logger::get_instance();
        auto &mappings = slot_mappings();
        auto &lastIds = last_applied_ids();

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

        // Stale-body guard. The load-detect thread stamps the
        // about-to-be-obsolete body pointer into swap_stale_comp()
        // when an in-session character swap fires before the engine
        // has rotated `user+0xD8` to the new body. If our resolver
        // still walks to that pre-rotation body, applying here would
        // paint the new character's preset onto the previous body
        // (e.g. on a Damiane save: Kliff's body ends up wearing
        // Damiane's outfit until the engine finally rotates and the
        // load-detect retry fires a fresh apply on the correct body).
        // Skipping returns control to the load-detect retry budget,
        // which will see the new component address on its next tick.
        // Apply paths that run on a body other than the stale one
        // clear the guard; save-load invalidation also clears it.
        {
            const auto staleComp =
                swap_stale_comp().load(std::memory_order_acquire);
            if (staleComp != 0 &&
                staleComp == static_cast<std::uintptr_t>(a1))
            {
                logger.debug(
                    "[dispatch] apply_all_transmog skipped: body still "
                    "pre-swap, a1={:#018x}, stale={:#018x}",
                    static_cast<uint64_t>(a1),
                    static_cast<uint64_t>(staleComp));
                return;
            }
        }

        // Suppress VEC hook for the entire operation.
        suppress_vec().store(true, std::memory_order_release);

        logger.trace("[dispatch] apply_all_transmog entry a1={:#018x}",
                     static_cast<uint64_t>(a1));

        // Body has rotated past the stale one (or no swap was in
        // flight) -- this apply will run against the live body, so
        // any captured stale value is now obsolete. CAS-clear under
        // the original captured value to avoid stomping a fresh
        // capture from a later swap that raced into the worker
        // between the early-out above and here.
        {
            auto staleComp =
                swap_stale_comp().load(std::memory_order_acquire);
            if (staleComp != 0 &&
                staleComp != static_cast<std::uintptr_t>(a1))
            {
                swap_stale_comp().compare_exchange_strong(
                    staleComp, 0,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
        }

        // Snapshot lastIds for diagnostic logging.
        const std::array<uint16_t, k_slotCount> prevIds = lastIds;

        // Early-out: skip all work if neither the preset nor the real
        // armor has changed since the last successful apply. Drops
        // spurious re-apply cycles fired by BatchEquip/VEC for non-armor
        // events (weapon swaps, ring changes, etc.).
        //
        // Slot tag order here must match k_tearDownSlots inside the
        // tear-down block: Helm(3), Chest(4), Gloves(5), Boots(6),
        // Cloak(16). TransmogSlot enum indices differ -- we translate.
        static constexpr std::uint16_t k_earlyOutSlotTags[5] = {
            0x0003, 0x0004, 0x0005, 0x0006, 0x0010};
        static constexpr TransmogSlot k_earlyOutSlots[5] = {
            TransmogSlot::Helm, TransmogSlot::Chest,
            TransmogSlot::Gloves, TransmogSlot::Boots,
            TransmogSlot::Cloak};

        std::array<std::uint16_t, 5> liveRealIds{};
        if (RealPartTearDown::is_ready())
        {
            for (std::size_t k = 0; k < 5; ++k)
            {
                liveRealIds[k] = RealPartTearDown::get_real_item_id(
                    reinterpret_cast<void *>(a1), k_earlyOutSlotTags[k]);
            }
        }

        std::array<bool, k_slotCount> slotNeedsWork{};
        {
            bool presetChanged = false;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                const auto &m = mappings[i];
                const uint16_t wouldBe =
                    (m.active && m.targetItemId != 0) ? m.targetItemId : 0;
                if (wouldBe != prevIds[i])
                {
                    presetChanged = true;
                    break;
                }
            }

            bool realChanged = false;
            for (std::size_t k = 0; k < 5; ++k)
            {
                if (liveRealIds[k] != last_applied_real_ids()[k])
                {
                    realChanged = true;
                    break;
                }
            }

            // Check for active "none" slots -- these need Phase B
            // tear-down and suppress reinforcement even when nothing
            // else changed (the game may re-equip real items after our
            // initial tear-down during the load sequence).
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
                logger.trace(
                    "apply_all_transmog: no state change "
                    "(prev=[{:#06x},{:#06x},{:#06x},{:#06x},{:#06x}] "
                    "real=[{:#06x},{:#06x},{:#06x},{:#06x},{:#06x}]), "
                    "skipping",
                    prevIds[0], prevIds[1], prevIds[2], prevIds[3], prevIds[4],
                    liveRealIds[0], liveRealIds[1], liveRealIds[2],
                    liveRealIds[3], liveRealIds[4]);
                suppress_vec().store(false, std::memory_order_release);
                return;
            }

            if (!presetChanged && realChanged)
            {
                logger.debug(
                    "apply_all_transmog: real armor changed, re-applying "
                    "(real=[{:#06x},{:#06x},{:#06x},{:#06x},{:#06x}] -> "
                    "[{:#06x},{:#06x},{:#06x},{:#06x},{:#06x}])",
                    last_applied_real_ids()[0], last_applied_real_ids()[1],
                    last_applied_real_ids()[2], last_applied_real_ids()[3],
                    last_applied_real_ids()[4],
                    liveRealIds[0], liveRealIds[1], liveRealIds[2],
                    liveRealIds[3], liveRealIds[4]);

                // Real swap means any previously-damaged slot now has
                // a NEW real item that is NOT damaged yet. Clear the
                // damage flags for slots whose real id changed so the
                // fake==real skip works correctly for the new real.
                for (std::size_t k = 0; k < 5; ++k)
                {
                    if (liveRealIds[k] != last_applied_real_ids()[k])
                    {
                        const auto idx =
                            static_cast<std::size_t>(k_earlyOutSlots[k]);
                        real_damaged()[idx] = false;
                    }
                }
            }

            // Per-slot "needs work" flags. Computed BEFORE overwriting
            // last_applied_real_ids so we compare against the previous
            // state. A slot needs tear-down + re-apply if its preset
            // target changed OR its underlying real item changed.
            // Unchanged slots are skipped -- no tear-down, no cache
            // clear, no SlotPopulator call -- so they don't flicker.
            //
            // Unticked slots whose real changes ARE still marked: a
            // prior restore via SlotPopulator may have left a dispatch
            // cache entry and scene-graph mesh. The cleanup pass after
            // the untick-restore loop relies on slotNeedsWork to find
            // and tear down these stale entries.
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                const auto &m = mappings[i];
                const uint16_t wouldBe =
                    (m.active && m.targetItemId != 0) ? m.targetItemId
                                                      : uint16_t{0};
                if (wouldBe != prevIds[i])
                {
                    slotNeedsWork[i] = true;
                    continue;
                }

                // Check if the real item changed for this slot.
                // Unticked slots still need cache cleanup when their
                // real changes -- an earlier restore via SlotPopulator
                // may have left a dispatch entry that the game's own
                // unequip flow can't remove.
                for (std::size_t k = 0; k < 5; ++k)
                {
                    if (static_cast<std::size_t>(k_earlyOutSlots[k]) == i &&
                        liveRealIds[k] != last_applied_real_ids()[k])
                    {
                        slotNeedsWork[i] = true;
                        break;
                    }
                }
                // Active "none" slots always need work for suppress
                // reinforcement.
                if (m.active && m.targetItemId == 0)
                    slotNeedsWork[i] = true;
            }

            // NOTE: last_applied_real_ids is updated at the END of the
            // function, after all applies succeed. If we crash mid-apply
            // (e.g. reload SEH), the old values remain so the next retry
            // detects the real-armor change and tries again.
        }

        // Clear lastIds for slots without a new target.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            auto &m = mappings[i];
            if (!m.active || m.targetItemId == 0)
                lastIds[i] = 0;
        }

        // SlotPopulator (sub_14076C960) maintains a dispatch cache on
        // the component at (basePtr, count, cap). The triple offsets
        // are defined by the k_compSlotCache* constants at the top of
        // this file (they shifted between v1.03.01 and v1.04.00). Each
        // entry is 24 bytes:
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
        // Fix: only clear subCount for dispatch cache entries whose
        // slotNativeId matches a slot we are about to re-apply. This
        // avoids nuking unchanged slots' blobs, which would force the
        // game to re-dispatch them through VEC and cause visible
        // flicker on gear that hasn't changed.
        //
        // Build a set of game tags that need clearing: any active slot
        // with a non-zero target, plus any unticked slot that needs
        // real-item restoration.
        std::uint16_t clearTags[k_slotCount]{};
        std::size_t clearTagCount = 0;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (slotNeedsWork[i])
                clearTags[clearTagCount++] = k_gameSlotTags[i];
        }

        __try
        {
            const auto count =
                *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
            const auto base =
                *reinterpret_cast<volatile uintptr_t *>(a1 + k_compSlotCacheBasePtrOffset);
            if (base > 0x10000)
            {
                for (uint32_t e = 0; e < count; ++e)
                {
                    const auto entry = base + 24ULL * e;
                    const auto slotId =
                        *reinterpret_cast<volatile uint16_t *>(entry);
                    for (std::size_t t = 0; t < clearTagCount; ++t)
                    {
                        if (slotId == clearTags[t])
                        {
                            *reinterpret_cast<volatile uint32_t *>(
                                entry + 0x10) = 0;
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
        // Both phases go through sub_14075FE60 which detaches particle
        // emitters / anim controllers from the scene graph via
        // sub_1425EBAE0. Auth table is NOT mutated.
        //
        // Game slot tags verified via CE hardware BP on sub_148EB6700:
        //   Helm=0x03 Chest=0x04 Gloves=0x05 Boots=0x06 Cloak=0x10.
        struct TearDownSlot
        {
            TransmogSlot slot;
            std::uint16_t gameTag;
        };
        static constexpr TearDownSlot k_tearDownSlots[] = {
            {TransmogSlot::Helm, 0x0003},
            {TransmogSlot::Chest, 0x0004},
            {TransmogSlot::Gloves, 0x0005},
            {TransmogSlot::Boots, 0x0006},
            {TransmogSlot::Cloak, 0x0010},
        };
        constexpr std::size_t k_tearDownCount =
            sizeof(k_tearDownSlots) / sizeof(k_tearDownSlots[0]);

        // Snapshot the real equipped itemId for each slot up front so both
        // phases and the PartShowSuppress mask can compare without
        // re-walking the auth table.
        std::uint16_t realItemId[k_tearDownCount]{};
        if (RealPartTearDown::is_ready())
        {
            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                realItemId[k] = RealPartTearDown::get_real_item_id(
                    reinterpret_cast<void *>(a1), k_tearDownSlots[k].gameTag);
            }

            // Phase A: previous fakes (from lastIds before this apply).
            // When the previous apply used a carrier, enable the
            // char-class bypass so the tear-down can trace the NPC
            // resource entries in the scene graph (they were loaded
            // with the bypass active).
            bool anyCarrierTearDown = false;
            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                const auto idx = static_cast<std::size_t>(
                    k_tearDownSlots[k].slot);
                if (!slotNeedsWork[idx])
                    continue;
                if (last_applied_carrier_ids()[idx] != 0 && prevIds[idx] != 0)
                {
                    anyCarrierTearDown = true;
                    break;
                }
            }

            const uintptr_t bypassAddr = resolved_addrs().charClassBypass;
            bool bypassForTearDown = false;
            if (anyCarrierTearDown)
            {
                bypassForTearDown =
                    set_char_class_bypass(bypassAddr, 0xEB);
                if (bypassForTearDown)
                    logger.trace("[carrier] charClass bypass ENABLED "
                                 "for Phase A tear-down");
            }

            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                const auto &td = k_tearDownSlots[k];
                const auto idx = static_cast<std::size_t>(td.slot);
                if (!slotNeedsWork[idx])
                    continue;
                const auto prevId = prevIds[idx];
                const auto prevCarrier = last_applied_carrier_ids()[idx];
                if (prevId == 0)
                    continue;
                // Phase A runs unconditionally: fake and real are
                // treated equally, so even when the previous fake
                // matched the live real item we still tear it down.
                if (prevCarrier != 0 &&
                    prevCarrier != static_cast<std::uint16_t>(prevId))
                {
                    logger.trace(
                        "[dispatch] tear_down_fake slot={:#06x} "
                        "carrier={:#06x} (then target={:#06x})",
                        td.gameTag, prevCarrier,
                        static_cast<std::uint16_t>(prevId));
                    RealPartTearDown::tear_down_by_item_id(
                        reinterpret_cast<void *>(a1),
                        prevCarrier,
                        td.gameTag);
                }
                RealPartTearDown::tear_down_by_item_id(
                    reinterpret_cast<void *>(a1),
                    static_cast<std::uint16_t>(prevId),
                    td.gameTag);
            }

            if (bypassForTearDown)
            {
                set_char_class_bypass(bypassAddr, 0x74);
                logger.trace("[carrier] charClass bypass RESTORED "
                             "after Phase A tear-down");
            }

            // Phase B: real items for any active slot. Runs
            // unconditionally -- fake and real are treated equally,
            // so even when the new fake matches the live real we
            // still tear down the real part. Only slots that had no
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
                if (RealPartTearDown::tear_down_real_part(
                        reinterpret_cast<void *>(a1), td.gameTag))
                {
                    real_damaged()[idx] = true;
                }
            }
        }

        // Helper: look up a slot's real itemId from the snapshot taken
        // during the tear-down phase.
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
                // Unchanged slot: preserve its lastIds entry but don't
                // re-apply. If it has a live dispatch cache entry the
                // game keeps rendering it.
                if (m.active && m.targetItemId != 0)
                    lastIds[i] = m.targetItemId;
                continue;
            }
            if (!m.active || m.targetItemId == 0)
                continue;

            // Fake and real are treated equally -- SlotPopulator runs
            // unconditionally even when the new fake itemId matches
            // the intact live real item. The earlier skip-on-match
            // fast-path was removed per user preference (predictable
            // apply sequence over incremental CPU savings).
            //
            // Decide: direct apply or carrier-assisted apply.
            const auto tmSlot = static_cast<TransmogSlot>(i);
            const auto targetId = m.targetItemId;
            const auto &activeChar =
                PresetManager::instance().active_character();
            const bool useCarrier = needs_carrier(targetId, activeChar);
            const uint16_t carrierId =
                useCarrier
                    ? default_carrier_for_slot(tmSlot, activeChar)
                    : 0;

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
                logger.debug("Transmog APPLY: slot={}, target={:#06x}",
                             slot_name(tmSlot), targetId);
                logger.trace("[dispatch] applying slot={} itemId={:#06x}",
                             i, targetId);
                apply_transmog(a1, targetId);
            }
            uint32_t postCount = 0;
            __try
            {
                postCount =
                    *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            logger.trace("[dispatch] post-apply slot={} liveCount={}",
                         i, postCount);
            lastIds[i] = m.targetItemId;
            last_applied_carrier_ids()[i] =
                (useCarrier && carrierId != 0) ? carrierId : 0;
            ++ourWrittenCount;
        }

        // When a slot's checkbox is UNTICKED (!m.active), it was
        // previously controlled by us (Phase B tore down the real
        // item). Restore the real item so it reappears.
        // "Active + none" (checkbox ticked, dropdown=none) means
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
                                slot_name(static_cast<TransmogSlot>(i)),
                                realId);
                    apply_transmog(a1, realId);
                    ++ourWrittenCount;
                }
                else if (real_damaged()[i])
                {
                    // Real item was unequipped (realId=0) but we
                    // previously restored it via SlotPopulator, which
                    // left a scene-graph mesh entry. Tear it down so
                    // the visual actually disappears. The old real ID
                    // is still in last_applied_real_ids (not yet
                    // overwritten -- moved to end of function).
                    for (std::size_t k = 0; k < k_tearDownCount; ++k)
                    {
                        if (static_cast<std::size_t>(
                                k_tearDownSlots[k].slot) != i)
                            continue;
                        const auto oldReal =
                            last_applied_real_ids()[k];
                        if (oldReal != 0)
                        {
                            logger.info(
                                "[dispatch] slot={} unticked + "
                                "unequipped -- tearing down restored "
                                "mesh {:#06x}",
                                slot_name(static_cast<TransmogSlot>(i)),
                                oldReal);
                            RealPartTearDown::tear_down_by_item_id(
                                reinterpret_cast<void *>(a1),
                                oldReal,
                                k_tearDownSlots[k].gameTag);
                        }
                        break;
                    }
                }
                // Clear damage flag so this slot is fully released
                // back to the game. Without this, future apply cycles
                // keep treating it as managed.
                real_damaged()[i] = false;
            }
            if (!m.active || m.targetItemId == 0)
                last_applied_carrier_ids()[i] = 0;
        }

        // Cleanup pass: tear down stale scene-graph meshes left by a
        // prior restore via SlotPopulator. This handles the case where
        // apply_single_slot restored the real item (creating a scene
        // graph entry), then the user unequipped in the game inventory.
        // The untick-restore block above doesn't catch this because
        // apply_single_slot already cleared prevIds and real_damaged.
        // We detect it here via slotNeedsWork + unticked + real=0 +
        // old real in last_applied_real_ids (not yet overwritten).
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
            const auto oldReal = last_applied_real_ids()[k];
            if (oldReal == 0)
                continue;
            logger.info("[dispatch] slot={} cleanup -- tearing down "
                        "stale restore mesh {:#06x}",
                        slot_name(td.slot), oldReal);
            RealPartTearDown::tear_down_by_item_id(
                reinterpret_cast<void *>(a1), oldReal, td.gameTag);
        }

        // Count was NOT zeroed -- unchanged slots' entries are still
        // live with their original subCount. Log the final state for
        // diagnostics.
        __try
        {
            uint32_t liveCount =
                *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
            logger.trace("[dispatch] post-apply liveCount={} "
                         "ourWrittenCount={}",
                         liveCount, ourWrittenCount);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        // targetMask: slots with a fake mesh to render. activeMask: slots
        // the user explicitly controls.
        std::uint32_t targetMask = 0;
        std::uint32_t activeMask = 0;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            if (lastIds[i] != 0)
                targetMask |= (std::uint32_t{1} << i);
            if (mappings[i].active)
                activeMask |= (std::uint32_t{1} << i);
        }

        // Suppression rule: suppress every active slot whose real item has
        // been (or will be) torn down. If the fake itemId matches the real
        // equipped id, the real item is still live and we must NOT
        // suppress it.
        std::uint32_t suppressMask = 0;
        for (std::size_t k = 0; k < k_tearDownCount; ++k)
        {
            const auto idx = static_cast<std::size_t>(k_tearDownSlots[k].slot);
            const auto &m = mappings[idx];
            if (!m.active)
                continue;
            if (m.targetItemId != 0 &&
                static_cast<std::uint16_t>(m.targetItemId) == realItemId[k])
                continue;
            suppressMask |= (std::uint32_t{1} << idx);
        }

        logger.info(
            "apply_all_transmog: prev=[{:#06x},{:#06x},{:#06x},{:#06x},{:#06x}] "
            "now=[{:#06x},{:#06x},{:#06x},{:#06x},{:#06x}] target={:#x} "
            "active={:#x} suppress={:#x}",
            prevIds[0], prevIds[1], prevIds[2], prevIds[3], prevIds[4],
            lastIds[0], lastIds[1], lastIds[2], lastIds[3], lastIds[4],
            targetMask, activeMask, suppressMask);

        PartShowSuppress::set_mask(static_cast<uint32_t>(suppressMask));

        // Commit the real-armor snapshot AFTER all applies succeed.
        // If the function crashed (SEH during reload), this line is
        // never reached and the next retry correctly detects the
        // real-armor change.
        last_applied_real_ids() = liveRealIds;

        suppress_vec().store(false, std::memory_order_release);
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

        // Snapshot the fakes we previously applied BEFORE clearing lastIds.
        struct ClearSlot
        {
            TransmogSlot slot;
            std::uint16_t gameTag;
        };
        static constexpr ClearSlot k_clearSlots[] = {
            {TransmogSlot::Helm, 0x0003},
            {TransmogSlot::Chest, 0x0004},
            {TransmogSlot::Gloves, 0x0005},
            {TransmogSlot::Boots, 0x0006},
            {TransmogSlot::Cloak, 0x0010},
        };
        std::uint16_t prevFakeId[std::size(k_clearSlots)]{};
        for (std::size_t k = 0; k < std::size(k_clearSlots); ++k)
        {
            const auto idx = static_cast<std::size_t>(k_clearSlots[k].slot);
            prevFakeId[k] = static_cast<std::uint16_t>(lastIds[idx]);
        }

        // Snapshot carrier IDs before clearing.
        std::uint16_t prevCarrierId[std::size(k_clearSlots)]{};
        for (std::size_t k = 0; k < std::size(k_clearSlots); ++k)
        {
            const auto idx = static_cast<std::size_t>(k_clearSlots[k].slot);
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
            for (std::size_t k = 0; k < std::size(k_clearSlots); ++k)
            {
                const auto fakeId = prevFakeId[k];
                const auto cId = prevCarrierId[k];
                if (fakeId == 0)
                    continue;
                const auto realId = RealPartTearDown::get_real_item_id(
                    reinterpret_cast<void *>(a1), k_clearSlots[k].gameTag);
                if (realId == fakeId && cId == 0)
                {
                    logger.trace(
                        "[clear] orphan-check slot={:#06x} fake={:#06x} "
                        "skipped (matches real, no carrier)",
                        k_clearSlots[k].gameTag, fakeId);
                    continue;
                }
                // Tear down carrier identity first if used.
                if (cId != 0 && cId != fakeId)
                {
                    logger.info(
                        "[clear] tearing carrier slot={:#06x} "
                        "carrier={:#06x} (real={:#06x})",
                        k_clearSlots[k].gameTag, cId, realId);
                    RealPartTearDown::tear_down_by_item_id(
                        reinterpret_cast<void *>(a1),
                        cId,
                        k_clearSlots[k].gameTag);
                }
                logger.info(
                    "[clear] tearing orphan fake slot={:#06x} itemId={:#06x} "
                    "(real={:#06x} carrier={:#06x})",
                    k_clearSlots[k].gameTag, fakeId, realId, cId);
                RealPartTearDown::tear_down_by_item_id(
                    reinterpret_cast<void *>(a1),
                    fakeId,
                    k_clearSlots[k].gameTag);
            }
        }
        else
        {
            logger.debug("[clear] RealPartTearDown not ready -- pass A skipped");
        }

        // Reset the real-item snapshot so the next apply re-reads from
        // the live auth table.
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

                    logger.info("Transmog RESTORE: real item {:#06x} for slot {}",
                                itemId, game_slot_name(gameSlot));
                    apply_transmog(a1, itemId);
                }

                // Restore count to the larger of saved and live.
                uint32_t liveCount =
                    *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset);
                uint32_t finalCount =
                    (liveCount > savedCount) ? liveCount : savedCount;
                *reinterpret_cast<volatile uint32_t *>(a1 + k_compSlotCacheCountOffset) = finalCount;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("Transmog clear exception during restore");
        }

        PartShowSuppress::clear_all_suppressed();

        logger.info("Transmog CLEAR: done");
        suppress_vec().store(false, std::memory_order_release);
    }

} // namespace Transmog
