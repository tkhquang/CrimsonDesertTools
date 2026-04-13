#include "transmog_apply.hpp"
#include "part_show_suppress.hpp"
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

    // Write the char-class bypass byte unconditionally. No from-check:
    // the read-after-write can see stale values when page protection
    // is restored between enable/restore cycles.
    static bool set_char_class_bypass(uintptr_t addr, uint8_t val) noexcept
    {
        if (!addr)
            return false;
        const auto byteVal = static_cast<std::byte>(val);
        return DMK::Memory::write_bytes(
            reinterpret_cast<std::byte *>(addr), &byteVal, 1).has_value();
    }

    // SEH-safe volatile memory readers. Isolated into noinline helpers
    // so the main apply logic can use C++ objects (lambdas, std::array)
    // without hitting MSVC C2712 ("cannot use __try in functions that
    // require object unwinding").
    __declspec(noinline) static uint32_t read_u32_vol(
        uintptr_t addr, bool &ok) noexcept
    {
        __try { ok = true; return *reinterpret_cast<volatile uint32_t *>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; return 0; }
    }

    // Forward declaration — apply_all_transmog delegates its core
    // work to this function so the bypass enable/restore can live in
    // the outer SEH frame (__try/__finally) while the inner function
    // is free to use C++ objects (MSVC C2712 prevents mixing __try
    // with objects that require unwinding in the same function).
    static void apply_all_transmog_inner(__int64 a1);

    // SEH-safe bypass wrapper. Enables the char-class bypass (jz→jmp),
    // runs the full apply cycle, then unconditionally restores jz via
    // __finally. This guarantees restore even if an SEH exception
    // propagates out of apply_all_transmog_inner (early-load faults,
    // game-internal page faults). Without __finally, a mid-cycle fault
    // would leave jmp permanently active, disabling the character-class
    // hash check for all actors until game restart.
    //
    // __finally (not __except): cleanup runs WITHOUT catching the
    // exception — game-internal SEH handlers still see it. __except
    // would intercept and break the game's own fault recovery.
    static void apply_with_bypass(__int64 a1, uintptr_t bypassAddr) noexcept
    {
        // Always attempt restore in __finally regardless of enable result.
        // write_bytes returns failure if VirtualProtect cannot restore the
        // original page protection AFTER successfully writing the byte.
        // Keying __finally on that return would skip the restore even though
        // 0xEB was actually written — leaving the bypass permanently active.
        set_char_class_bypass(bypassAddr, 0xEB);

        __try
        {
            apply_all_transmog_inner(a1);
        }
        __finally
        {
            set_char_class_bypass(bypassAddr, 0x74);
        }
    }

    void apply_all_transmog(__int64 a1)
    {
        auto &logger = DMK::Logger::get_instance();
        auto &mappings = slot_mappings();
        auto &lastIds = last_applied_ids();

        if (world_system_ptr().load(std::memory_order_acquire))
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

        // Suppress VEC hook for the entire operation.
        suppress_vec().store(true, std::memory_order_release);

        logger.trace("[dispatch] apply_all_transmog entry a1={:#018x}",
                     static_cast<uint64_t>(a1));

        // Snapshot lastIds for diagnostic logging.
        const std::array<uint16_t, k_slotCount> prevIds = lastIds;

        // Early-out: skip all work if neither the preset nor the real
        // armor has changed since the last successful apply. Drops
        // spurious re-apply cycles fired by BatchEquip/VEC for non-armor
        // events (weapon swaps, ring changes, etc.).
        //
        // Slot tag order here must match k_tearDownSlots inside the
        // tear-down block: Helm(3), Chest(4), Gloves(5), Boots(6),
        // Cloak(16). TransmogSlot enum indices differ — we translate.
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

            // Check for active "none" slots — these need Phase B
            // tear-down and suppress reinforcement even when nothing
            // else changed (the game may re-equip real items after our
            // initial tear-down during the load sequence).
            bool hasActiveNone = false;
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                if (mappings[i].active && mappings[i].targetItemId == 0)
                { hasActiveNone = true; break; }
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

            last_applied_real_ids() = liveRealIds;
        }

        // SlotPopulator (sub_14076C960) maintains a dispatch cache at
        // a1+0x1B8 (base ptr), a1+0x1C0 (count), a1+0x1C4 (cap). Each
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
        //      existing subArray — the stale blob still gets dispatched
        //      to VEC via sub_14076D520, producing the stale helm bug.
        //  (2) Even on a slot we ARE re-populating this frame, the game
        //      never clears subCount, so the previously queued blob
        //      lingers and gets replayed alongside the new one.
        //
        // Fix: walk every existing cache entry while count is still
        // valid and force-clear subCount=0 (defensive, O(n<=16)). Then
        // zero count so SlotPopulator allocates fresh entries for our
        // targeted slots. Finally restore count to OUR written count,
        // not the saved one, so prior-preset entries stay invisible.
        uint32_t savedCount = 0;
        uintptr_t dispatchBase = 0;
        __try
        {
            savedCount = *reinterpret_cast<volatile uint32_t *>(a1 + 0x1C0);
            dispatchBase = *reinterpret_cast<volatile uintptr_t *>(a1 + 0x1B8);

            if (dispatchBase > 0x10000)
            {
                for (uint32_t e = 0; e < savedCount; ++e)
                {
                    auto entry = dispatchBase + 24ULL * e;
                    *reinterpret_cast<volatile uint32_t *>(entry + 0x10) = 0;
                }
            }
            *reinterpret_cast<volatile uint32_t *>(a1 + 0x1C0) = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("[dispatch] cache clear fault");
            suppress_vec().store(false, std::memory_order_release);
            return;
        }

        // Delegate to apply_with_bypass which wraps the core work in
        // __try/__finally to guarantee the char-class bypass byte is
        // restored even on SEH exceptions (early-load faults, etc).
        apply_with_bypass(a1, resolved_addrs().charClassBypass);

        suppress_vec().store(false, std::memory_order_release);
    }

    static void apply_all_transmog_inner(__int64 a1)
    {
        auto &logger = DMK::Logger::get_instance();
        auto &mappings = slot_mappings();
        auto &lastIds = last_applied_ids();

        // Snapshot BEFORE clearing so Phase A tear-down knows which
        // fakes to remove (e.g. Antra helm → "none" transition).
        const std::array<uint16_t, k_slotCount> prevIds = lastIds;

        // Clear lastIds for slots without a new target. Must happen
        // AFTER the prevIds snapshot above — Phase A needs the old IDs.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const auto &m = mappings[i];
            if (!m.active || m.targetItemId == 0)
                lastIds[i] = 0;
        }

        // Two-phase scene-graph tear-down before applying fakes.
        //
        // Phase A — tear down the previous preset's fake meshes using
        //   lastIds[] as the itemId source.
        // Phase B — tear down the REAL item in the auth table for every
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

            // Phase A: tear down previous fakes (from lastIds before
            // this apply). Skip items that match the real equipped id
            // — those are still live in the scene graph.
            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                const auto &td = k_tearDownSlots[k];
                const auto idx = static_cast<std::size_t>(td.slot);
                const auto prevId = prevIds[idx];
                if (prevId == 0)
                    continue;
                if (static_cast<std::uint16_t>(prevId) == realItemId[k])
                {
                    logger.trace(
                        "[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} "
                        "skipped (matches real)",
                        td.gameTag,
                        static_cast<std::uint16_t>(prevId));
                    continue;
                }
                RealPartTearDown::tear_down_by_item_id(
                    reinterpret_cast<void *>(a1),
                    static_cast<std::uint16_t>(prevId),
                    td.gameTag);
            }

            // Phase B: real items for any active slot. Skip slots where
            // the NEW fake itemId matches the real equipped id AND the
            // real is still intact.
            for (std::size_t k = 0; k < k_tearDownCount; ++k)
            {
                const auto &td = k_tearDownSlots[k];
                const auto idx = static_cast<std::size_t>(td.slot);
                auto &m = mappings[idx];
                if (!m.active)
                    continue;
                if (m.targetItemId != 0 &&
                    static_cast<std::uint16_t>(m.targetItemId) == realItemId[k] &&
                    !real_damaged()[idx])
                {
                    logger.trace(
                        "[dispatch] tear_down slot={:#06x} itemId={:#06x} "
                        "skipped (fake matches intact real)",
                        td.gameTag,
                        static_cast<std::uint16_t>(m.targetItemId));
                    continue;
                }
                if (RealPartTearDown::tear_down_real_part(
                        reinterpret_cast<void *>(a1), td.gameTag))
                {
                    real_damaged()[idx] = true;
                }
            }
        }

        // Helper: look up a slot's real itemId from the snapshot taken
        // during the tear-down phase.
        auto lookup_real_id = [&](std::size_t slotIdx) -> std::uint16_t {
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
            if (!m.active || m.targetItemId == 0)
                continue;

            // Skip SlotPopulator when the fake matches the INTACT live
            // real item. Once the real has been damaged by a previous
            // phase-B tear-down, fall through so SlotPopulator can
            // restore the mesh — the layer-2 effects are already lost.
            //
            // Important: do NOT increment ourWrittenCount on the skip
            // path. We did not actually write a dispatch cache entry.
            const std::uint16_t realId = lookup_real_id(i);
            if (realId != 0 &&
                static_cast<std::uint16_t>(m.targetItemId) == realId &&
                !real_damaged()[i])
            {
                logger.trace(
                    "[dispatch] apply_transmog slot={} itemId={:#06x} "
                    "skipped (fake matches intact real)",
                    i, m.targetItemId);
                lastIds[i] = m.targetItemId;
                continue;
            }

            const auto tmSlot = static_cast<TransmogSlot>(i);
            const auto targetId = m.targetItemId;

            logger.debug("Transmog APPLY: slot={}, target={:#06x}",
                         slot_name(tmSlot), targetId);
            logger.trace("[dispatch] applying slot={} itemId={:#06x}",
                         i, targetId);
            apply_transmog(a1, targetId);

            bool countOk = false;
            const uint32_t postCount = read_u32_vol(
                static_cast<uintptr_t>(a1) + 0x1C0, countOk);
            logger.trace("[dispatch] post-apply slot={} liveCount={}",
                         i, postCount);
            lastIds[i] = m.targetItemId;
            ++ourWrittenCount;
        }

        // When a slot's checkbox is UNTICKED (!m.active), it was
        // previously controlled by us (Phase B tore down the real
        // item). Restore the real item so it reappears.
        // "Active + none" (checkbox ticked, dropdown=none) means
        // "show empty" — do NOT restore.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const auto &m = mappings[i];
            if (!m.active && (prevIds[i] != 0 || real_damaged()[i]))
            {
                const std::uint16_t realId = lookup_real_id(i);
                if (realId != 0)
                {
                    logger.info("[dispatch] slot={} unticked — "
                                "restoring real item {:#06x}",
                                slot_name(static_cast<TransmogSlot>(i)),
                                realId);
                    apply_transmog(a1, realId);
                    ++ourWrittenCount;
                }
            }
        }

        // Restore count to OUR writes only — stale entries in the
        // range [ourWrittenCount .. savedCount) remain allocated
        // (no leak) but are invisible to SlotPopulator's count-limited
        // linear search.
        {
            bool countOk = false;
            const uint32_t liveCount = read_u32_vol(
                static_cast<uintptr_t>(a1) + 0x1C0, countOk);
            if (countOk)
            {
                logger.trace("[dispatch] liveCount={} ourWrittenCount={}",
                             liveCount, ourWrittenCount);
                const uint32_t finalCount =
                    (liveCount > ourWrittenCount) ? liveCount : ourWrittenCount;
                *reinterpret_cast<volatile uint32_t *>(a1 + 0x1C0) = finalCount;
            }
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
    }

    void clear_all_transmog(__int64 a1)
    {
        auto &logger = DMK::Logger::get_instance();
        auto &lastIds = last_applied_ids();

        if (world_system_ptr().load(std::memory_order_acquire))
        {
            auto fresh = resolve_player_component();
            if (fresh > 0x10000) a1 = fresh;
        }

        suppress_vec().store(true, std::memory_order_release);

        // Snapshot the fakes we previously applied BEFORE clearing lastIds.
        struct ClearSlot
        {
            TransmogSlot slot;
            std::uint16_t gameTag;
        };
        static constexpr ClearSlot k_clearSlots[] = {
            {TransmogSlot::Helm,   0x0003},
            {TransmogSlot::Chest,  0x0004},
            {TransmogSlot::Gloves, 0x0005},
            {TransmogSlot::Boots,  0x0006},
            {TransmogSlot::Cloak,  0x0010},
        };
        std::uint16_t prevFakeId[std::size(k_clearSlots)]{};
        for (std::size_t k = 0; k < std::size(k_clearSlots); ++k)
        {
            const auto idx = static_cast<std::size_t>(k_clearSlots[k].slot);
            prevFakeId[k] = static_cast<std::uint16_t>(lastIds[idx]);
        }

        // Clear lastIds and per-slot damage flags.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            lastIds[i] = 0;
            real_damaged()[i] = false;
        }

        // Pass A: tear down orphan fakes.
        if (RealPartTearDown::is_ready())
        {
            for (std::size_t k = 0; k < std::size(k_clearSlots); ++k)
            {
                const auto fakeId = prevFakeId[k];
                if (fakeId == 0)
                    continue;
                const auto realId = RealPartTearDown::get_real_item_id(
                    reinterpret_cast<void *>(a1), k_clearSlots[k].gameTag);
                if (realId == fakeId)
                {
                    logger.trace(
                        "[clear] orphan-check slot={:#06x} fake={:#06x} "
                        "skipped (matches real)",
                        k_clearSlots[k].gameTag, fakeId);
                    continue;
                }
                logger.info(
                    "[clear] tearing orphan fake slot={:#06x} itemId={:#06x} "
                    "(real={:#06x})",
                    k_clearSlots[k].gameTag, fakeId, realId);
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
            auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + 120);
            if (entryDesc > 0x10000)
            {
                auto entryArray = *reinterpret_cast<uintptr_t *>(entryDesc + 8);
                auto entryCount = *reinterpret_cast<uint32_t *>(entryDesc + 16);

                auto savedCount = *reinterpret_cast<uint32_t *>(a1 + 0x1C0);
                *reinterpret_cast<uint32_t *>(a1 + 0x1C0) = 0;

                for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
                {
                    auto base = entryArray + e * 200;
                    auto gameSlot = *reinterpret_cast<int16_t *>(base + 192);
                    auto itemId = *reinterpret_cast<uint16_t *>(base + 8);

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
                    *reinterpret_cast<volatile uint32_t *>(a1 + 0x1C0);
                uint32_t finalCount =
                    (liveCount > savedCount) ? liveCount : savedCount;
                *reinterpret_cast<volatile uint32_t *>(a1 + 0x1C0) = finalCount;
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
