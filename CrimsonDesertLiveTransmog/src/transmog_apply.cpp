#include "transmog_apply.hpp"
#include "auth_table.hpp"
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
#include "transmog_map.hpp"
#include "transmog_worker.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace Transmog
{
    // Floor for anything treated as a live pointer. Below this is a packed scalar, a small index, or a null.
    //
    // Signed, because the engine hands component pointers through `__int64` parameters. Comparing one of those
    // against an UNSIGNED floor converts the pointer instead, so a negative (garbage) value compares ABOVE the floor
    // and passes the very guard that exists to reject it.
    constexpr std::int64_t k_minPlausiblePtr = 0x10000;

    // Candidate-exclusion list on the equip-slot component. The item -> slot resolver walks the character's candidate
    // slots and returns the first that validates, and the validator rejects any candidate found in the WORD array at
    // `k_compSlotExcludeListOffset` (count at `k_compSlotExcludeCountOffset`). The pair is empty in normal play, which
    // is what makes it safe to borrow for one equip. It moves with the same component width as the cache triple below.
    constexpr std::ptrdiff_t k_compSlotExcludeListOffset = 104;
    constexpr std::ptrdiff_t k_compSlotExcludeCountOffset = 112;

    // SlotPopulator maintains a dispatch cache on the component at (basePtr, count, cap). The triple moves as one unit
    // whenever the component gains or loses fields below it. All three constants must move together, and the same
    // component width also drives the auth-table container pointer in auth_table.hpp. Verify them against the live
    // SlotPopulator body on patch day. That body forms one base pointer and reads the other two members off it:
    //   lea  r15, [a1+k_compSlotCacheBasePtrOffset]
    //   mov  r8d, [r15+0x08]     ; count
    //   mov  r9,  [r15]          ; basePtr
    // The later grow check pins the capacity directly instead of by adjacency:
    //   mov ecx,[r15+0x0C] ; mov eax,[r15+0x08] ; cmp ecx,eax ; ja
    //
    // A stale triple fails SILENTLY in two directions. If the count offset lands on a neighboring u32 that is always
    // 0, apply looks like a no-op. If it lands on the LOW 32 bits of the basePtr qword, the read yields a wildly
    // inflated count -- the low bits of a heap address. A write at that offset then shreds the dispatch-cache pointer.
    // Writes at stale offsets also scribble into adjacent fields and corrupt the component one slot at a time.
    constexpr std::ptrdiff_t k_compSlotCacheBasePtrOffset = 0x1D8;
    constexpr std::ptrdiff_t k_compSlotCacheCountOffset = 0x1E0;
    constexpr std::ptrdiff_t k_compSlotCacheCapOffset = 0x1E4;

    // Auth-table geometry (container pointer, entry stride, field offsets) lives in auth_table.hpp -- one copy for the
    // whole mod, because the whole struct moves as a unit on patch day.

    // (TransmogSlot, engine slot tag) pairs the dispatcher iterates for tear-down + the auth-table real-id snapshot.
    // Sourced from slot_metadata.hpp's single per-slot table. The local TearDownSlot alias keeps existing call sites
    // (`td.slot`, `td.gameTag`) reading unchanged. Order matches the TransmogSlot enum.
    using TearDownSlot = SlotMetadata;
    static constexpr auto &k_tearDownSlots = k_slotMetadata;
    static constexpr std::size_t k_tearDownCount = k_slotCount;

    // Walk the auth-table for the entry whose +0xC8 slotTag matches `gameTag`, snapshot its dye-record vector, and
    // publish through DyeRecordInject so the next apply_transmog -> SlotPopulator -> DyeCopier round-trip emits exactly
    // those records into the render struct's dst+120. Returns true when it published records. The caller must call
    // clear_slot_dye_state after the apply pass.
    //
    // Used ONLY by the untick-restore branch in apply_all_transmog (`!m.active && prevIds != 0`). When the user
    // unticks a slot and the real item returns to view, this repaints it in its current inventory dye instead of its
    // factory palette.
    //
    // Fakes with no explicit preset dye flow through with their natural engine records. Monster-carrier fakes whose
    // engine source is empty render colorless. To seed preset dye for those fakes the user must call Capture Outfit
    // (mass) or the per-slot "Sync from live" button in the dye popup, which are the only paths that mutate the active
    // preset.
    static bool publish_entry_dye_for_gameslot(__int64 a1, std::int16_t gameTag) noexcept
    {
        uintptr_t entryBase = 0;
        __try
        {
            const auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + AuthTable::k_containerPtrOffset);
            if (entryDesc < 0x10000)
                return false;
            const auto entryArray = *reinterpret_cast<uintptr_t *>(entryDesc + AuthTable::k_containerArrayBaseOffset);
            const auto entryCount = *reinterpret_cast<uint32_t *>(entryDesc + AuthTable::k_containerCountOffset);
            for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
            {
                const auto base = entryArray + e * AuthTable::k_entryStride;
                const auto sl = *reinterpret_cast<int16_t *>(base + AuthTable::k_entrySlotTagOffset);
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
        // Sparse mode: emit only the channels that the auth-table entry carried. Dense fill uses the first active
        // channel as a fallback, so it paints mesh sub-parts (e.g. cloak facings) that the real item never colored.
        DyeRecordInject::set_slot_dye_state(live, /*sparse=*/true);
        return true;
    }

    // The engine calls below live in POD-only wrappers because MSVC forbids `__try` in a frame that needs object
    // unwinding, and the apply path formats log strings. Each returns a failure value rather than letting a fault
    // escape: an unguarded fault here aborts the whole apply, so one bad slot would take every other slot with it.

    /// POD-only SEH wrapper: ask the engine where it would place `itemId`, or `k_noGameTag` if nowhere.
    static std::uint16_t item_to_slot_seh(ItemToSlotResolveFn fn, std::int64_t a1, std::uint16_t itemId) noexcept
    {
        if (!fn)
            return k_noGameTag;
        __try
        {
            return static_cast<std::uint16_t>(fn(a1, static_cast<std::int16_t>(itemId)) & 0xFFFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return k_noGameTag;
        }
    }

    /**
     * Temporarily exclude a slot from the engine's item -> slot resolution.
     *
     * The engine's item -> slot resolver walks the character's candidate slots and returns the FIRST that validates.
     * The validator rejects any candidate listed in the exclusion array (see k_compSlotExcludeListOffset), and that
     * array is empty in normal play. Excluding the first half of a pair for the duration of one equip therefore makes
     * the resolver fall through to the second -- otherwise unreachable, because both halves share one item type and
     * the first always wins.
     *
     * Touches no equip state: this is a transient resolution filter, not the auth table.
     *
     * @return false when the list is already populated, so a live exclusion set is never displaced. POD-only frame so
     *         the SEH guard is legal.
     */
    static bool arm_slot_exclusion_seh(std::int64_t a1, std::uint16_t *buf, std::uint16_t excludeTag,
                                       std::uint64_t &savedPtr, std::uint32_t &savedCount) noexcept
    {
        if (a1 < k_minPlausiblePtr || !buf || excludeTag == k_noGameTag)
            return false;
        __try
        {
            savedPtr = *reinterpret_cast<std::uint64_t *>(a1 + k_compSlotExcludeListOffset);
            savedCount = *reinterpret_cast<std::uint32_t *>(a1 + k_compSlotExcludeCountOffset);
            if (savedCount != 0)
                return false; // something already uses it -- do not displace
            *buf = excludeTag;
            *reinterpret_cast<std::uint64_t *>(a1 + k_compSlotExcludeListOffset) =
                reinterpret_cast<std::uint64_t>(buf);
            *reinterpret_cast<std::uint32_t *>(a1 + k_compSlotExcludeCountOffset) = 1;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    /// Undo @ref arm_slot_exclusion_seh. Must run on every path out, including an SEH unwind through the equip.
    static void disarm_slot_exclusion_seh(std::int64_t a1, std::uint64_t savedPtr, std::uint32_t savedCount) noexcept
    {
        if (a1 < k_minPlausiblePtr)
            return;
        __try
        {
            *reinterpret_cast<std::uint64_t *>(a1 + k_compSlotExcludeListOffset) = savedPtr;
            *reinterpret_cast<std::uint32_t *>(a1 + k_compSlotExcludeCountOffset) = savedCount;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    /// True when `slotTag` names a live part record. POD-only frame so the SEH guard is legal.
    static bool slot_tag_is_live_seh(SlotTagToHandleFn resolve, __int64 a1, std::uint16_t slotTag) noexcept
    {
        if (!resolve)
            return false;
        __try
        {
            std::uint16_t handle = k_noGameTag;
            resolve(a1, &handle, slotTag, 0);
            return handle != k_noGameTag;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    /**
     * @brief Resolve the slot tag to its handle, then refresh -- under SEH, in a POD-only frame.
     *
     * @details PartSlotRefresh's two slot arguments are NOT the same namespace. The first is a TAG, matched against
     *          bucket keys and against the part record's own tag field. The second is a HANDLE, which the function
     *          dereferences through a lookup. Passing the tag for both faults.
     * @return false when either pointer is null, the tag names no live part record, or the call faulted.
     */
    static bool call_part_slot_refresh_seh(PartSlotRefreshFn fn, SlotTagToHandleFn resolve, __int64 a1,
                                           std::uint16_t slotTag, __int64 swapEntry) noexcept
    {
        if (!fn || !resolve)
            return false;
        __try
        {
            std::uint16_t handle = k_noGameTag;
            resolve(a1, &handle, slotTag, 0);
            if (handle == k_noGameTag)
                return false; // tag names no live part record -- nothing to refresh
            fn(a1, static_cast<__int16>(slotTag), static_cast<__int16>(handle), swapEntry);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The SlotPopulator choke point for every apply.
    //
    // `id` is the descriptor id fed to the engine -- always an item the wearer can legitimately equip, because the
    // transmog VISUAL no longer comes from the descriptor. It comes from the prefab-wrapper swap, which redirects the
    // mesh this item would otherwise render. Nothing here has to defeat an equip gate.
    //
    // `slotSel` chooses WHICH engine slot receives the item. SlotPopulator reads the u16 at itemData+12, and
    // `k_noGameTag` means "derive the slot from the item", which the engine resolves through an item -> slot lookup.
    // For a PAIRED slot that derivation can only ever produce one answer -- both halves share one item type -- so the
    // second half is unreachable and its carrier lands in the first. Naming the slot explicitly reaches the other half.
    static void apply_transmog_core(__int64 a1, uint16_t id, uint16_t slotSel = k_noGameTag,
                                    uint16_t excludeTag = k_noGameTag)
    {
        auto slotPop = slot_populator_fn();
        auto initEntry = init_swap_entry_fn();
        if (!slotPop || !initEntry)
            return;

        // Build 16-byte item data structure for SlotPopulator. Layout matches a natural-engine equip exactly:
        //   XX YY 02 00 00 00 00 00 FF FF FF FF FF FF 00 00
        // The engine validates the +4..+11 region as part of its dye/material-instance lookup. If those dwords are
        // reordered, the engine falls back to default colors even when the wrapper-swap mesh is correct.
        // Settle the destination BEFORE the struct is built -- itemData+12 is written below, and a later change to
        // this variable would not reach the engine.
        //
        // An explicit destination needs the slot to already own a part record: SlotPopulator resolves the tag first
        // and bails outright when it cannot, equipping nothing. Fall back to derivation rather than refuse.
        bool excludeFirstHalf = false;
        if (slotSel != k_noGameTag && !slot_tag_is_live_seh(slot_tag_to_handle_fn(), a1, slotSel))
        {
            // Derivation alone would land on the FIRST half of the pair. Excluding that half from resolution makes
            // it land here instead, without naming a destination the engine would refuse.
            excludeFirstHalf = (excludeTag != k_noGameTag);
            if (excludeFirstHalf)
                DMK::Logger::get_instance().debug(
                    "[dispatch] slot {:#06x} has no live part record -- deriving with {:#06x} excluded", slotSel,
                    excludeTag);
            else
                DMK::Logger::get_instance().debug(
                    "[dispatch] slot {:#06x} has no live part record -- deriving", slotSel);
            slotSel = k_noGameTag;
        }

        alignas(16) uint8_t itemData[16]{};
        *reinterpret_cast<uint16_t *>(itemData + 0) = id;
        itemData[2] = 2;
        // bytes 4..7 left as 0 (zero-init)
        *reinterpret_cast<uint32_t *>(itemData + 8) = 0xFFFFFFFF;
        *reinterpret_cast<uint16_t *>(itemData + 12) = slotSel;

        // Build empty swap entry.
        alignas(16) uint8_t swapEntry[256]{};
        initEntry(reinterpret_cast<__int64>(swapEntry));

        in_transmog().store(true, std::memory_order_relaxed);
        // Reset the host-scope cluster so the upcoming slotPop's matInst-iter hits build a fresh player-vs-NPC
        // histogram.
        ColorOverride::HostScope::begin_apply_window();
        // Open the setter-property substitute window. Any 4-byte material-property write the engine fires during
        // slotPop goes to the user's chosen RGB. The window closes again immediately after, so unrelated render passes
        // are not tinted.
        ColorOverride::SetterSubstitute::set_apply_window(true);
        // slotPop faults (structured exception) on early load before game data is ready. __finally restores the apply
        // window and the in_transmog() flag even on an SEH unwind. A stranded apply window tints unrelated render
        // passes; a stranded in_transmog() leaves the wrapper swap armed outside its window.
        //
        // The return is CAPTURED, not discarded: SlotPopulator answers `k_noGameTag` when it refuses -- including the
        // case where an explicit slotSel does not resolve, where it bails before equipping anything. Discarding it
        // makes every such refusal silent and indistinguishable from a successful equip whose visual did not change.
        //
        // `slotPopCompleted` is what separates a REFUSAL from a FAULT. slotPopRc starts at -1, and the low word of -1
        // is the same `k_noGameTag` a refusal returns, so without the flag a fault inside the call -- which skips the
        // assignment entirely -- reports as "REFUSED, nothing was equipped": identical symptom, different cause.
        alignas(2) std::uint16_t exclusionBuf = k_noGameTag;
        std::uint64_t savedExclPtr = 0;
        std::uint32_t savedExclCount = 0;
        const bool exclusionArmed =
            excludeFirstHalf && arm_slot_exclusion_seh(a1, &exclusionBuf, excludeTag, savedExclPtr, savedExclCount);

        std::int64_t slotPopRc = -1;
        bool slotPopCompleted = false;
        bool refreshCalled = false;
        bool refreshFaulted = false;
        const auto refreshFn = part_slot_refresh_fn();
        __try
        {
            slotPopRc =
                slotPop(a1, reinterpret_cast<unsigned __int16 *>(itemData), reinterpret_cast<__int64>(swapEntry));
            slotPopCompleted = true;

            // Refresh the slot we actually targeted -- INSIDE the apply window, exactly where the engine does its
            // own.
            //
            // SlotPopulator files the entry under `slotSel` but rebuilds the slot it DERIVED FROM THE ITEM. Both
            // halves of a paired slot derive the same value, so the second half's entry lands correctly while the
            // first half is what gets rebuilt. Repeating the rebuild with our slot in BOTH argument positions covers
            // the half the engine skipped.
            //
            // The placement matters as much as the call. Run after the __finally below, this rebuilds with the dye
            // substitute window shut and in_transmog() already cleared: the prefab swap does not substitute and the
            // dye writes are not intercepted, so the part flickers to its untransmogged mesh and armor loses its
            // colour a moment after appearing. Both windows have to still be open.
            //
            // Only for an explicit slotSel -- with `k_noGameTag` the engine's own derivation is already right.
            if (slotSel != k_noGameTag && static_cast<std::uint16_t>(slotPopRc & 0xFFFF) != k_noGameTag && refreshFn)
            {
                refreshCalled = call_part_slot_refresh_seh(refreshFn, slot_tag_to_handle_fn(), a1, slotSel,
                                                           reinterpret_cast<__int64>(swapEntry));
                refreshFaulted = !refreshCalled;
            }
        }
        __finally
        {
            ColorOverride::SetterSubstitute::set_apply_window(false);
            in_transmog().store(false, std::memory_order_relaxed);
        }

        // Always restore, including on an SEH unwind through the block above.
        if (exclusionArmed)
            disarm_slot_exclusion_seh(a1, savedExclPtr, savedExclCount);

        // Logging lives outside the __try: string formatting needs object unwinding, which cannot coexist with SEH
        // in the same frame.
        const auto rcWord = static_cast<std::uint16_t>(slotPopRc & 0xFFFF);
        if (!slotPopCompleted)
        {
            DMK::Logger::get_instance().warning(
                "[dispatch] SlotPopulator FAULTED item={:#06x} slotSel={:#06x} -- the call raised, it did not refuse",
                id, slotSel);
        }
        else if (rcWord == k_noGameTag)
        {
            DMK::Logger::get_instance().warning(
                "[dispatch] SlotPopulator REFUSED item={:#06x} slotSel={:#06x} -- nothing was equipped", id, slotSel);
        }
        else
        {
            DMK::Logger::get_instance().trace("[dispatch] SlotPopulator ok item={:#06x} slotSel={:#06x} refresh={}",
                                              id, slotSel, refreshFaulted ? "FAULTED" : (refreshCalled ? "yes" : "no"));
        }
    }

    /**
     * @brief Drive the engine's per-slot rebuild with the prefab-swap and colour windows open.
     *
     * @details Internal on purpose. It publishes NO dye of its own, so calling it after the apply path has cleared
     *          the dye state makes the rebuild's DyeCopier call re-emit the engine's natural records and strip the
     *          injected colour. Every caller goes through @ref refresh_slot_appearance, which brackets it with the
     *          dye publish and the ColorOverride slot bind.
     * @return false when an anchor is unresolved, the slot has no live part record, or the call faulted.
     */
    static bool refresh_slot_visual(TransmogSlot slot)
    {
        auto &logger = DMK::Logger::get_instance();

        const auto a1 = static_cast<__int64>(player_a1().load(std::memory_order_acquire));
        if (!a1)
            return false;
        const auto tag = game_slot_from_transmog(slot);
        if (tag < 0)
            return false;

        const auto refresh = part_slot_refresh_fn();
        const auto resolve = slot_tag_to_handle_fn();
        const auto initEntry = init_swap_entry_fn();
        if (!refresh || !resolve || !initEntry)
            return false;

        // An EMPTY swap entry on purpose. PartSlotRefresh falls back to the entry already registered for the slot
        // when its 4th argument is the sentinel or empty, so a rebuild needs no item id and no equip -- it reuses
        // whatever is installed and just re-runs the build.
        alignas(16) std::uint8_t swapEntry[256]{};
        initEntry(reinterpret_cast<__int64>(swapEntry));

        // Both windows open, exactly as an apply does. in_transmog keeps the prefab swap substituting, so the
        // transmogged mesh survives the rebuild instead of reverting to the carrier; the setter window routes the
        // engine's material writes to the chosen colour.
        //
        // This ordering is the whole point: running a rebuild with these shut is what made armor come up dyed and
        // then revert a moment later.
        in_transmog().store(true, std::memory_order_relaxed);
        ColorOverride::HostScope::begin_apply_window();
        ColorOverride::SetterSubstitute::set_apply_window(true);

        const bool ok =
            call_part_slot_refresh_seh(refresh, resolve, a1, static_cast<std::uint16_t>(tag),
                                       reinterpret_cast<__int64>(swapEntry));

        ColorOverride::SetterSubstitute::set_apply_window(false);
        in_transmog().store(false, std::memory_order_relaxed);

        logger.debug("[dispatch] refresh_slot_visual slot={} tag={:#06x} -> {}", slot_name(slot),
                     static_cast<std::uint16_t>(tag), ok ? "ok" : "failed");
        return ok;
    }

    bool refresh_slot_appearance(std::size_t slotIdx)
    {
        if (slotIdx >= k_slotCount)
            return false;
        const auto slot = static_cast<TransmogSlot>(slotIdx);

        // Publish the same dye state a full apply would, then rebuild instead of re-equipping. The injector's detour
        // consumes this on the next DyeCopier call, which the rebuild drives -- so the records land without the slot
        // being torn down and equipped again.
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
            DyeRecordInject::clear_slot_dye_state();
        }

        ColorOverride::SetterSubstitute::set_active_slot(static_cast<int>(slotIdx));
        const bool ok = refresh_slot_visual(slot);
        DyeRecordInject::clear_slot_dye_state();

        DMK::Logger::get_instance().debug("[dispatch] refresh_slot_appearance slot={} -> {}", slot_name(slot),
                                          ok ? "rebuilt" : "unavailable");
        return ok;
    }

    void apply_transmog(__int64 a1, uint16_t targetId)
    {
        apply_transmog_core(a1, targetId);
    }

    // -- Default carrier set (per character) -----------------------------
    // Each entry must be a valid item for THAT character in the given slot -- something the engine's equip class-gate
    // accepts. Kliff and Oongka share Kliff_PlateArmor_* because the engine treats their equip class the same. Damiane
    // has her own armor namespace (Demian_*) which Kliff cannot wear, but the engine accepts those items on Damiane
    // even though the item catalog marks them PlayerSafe=no. The PlayerSafe flag reflects Kliff compatibility only.
    //
    // If a name fails to resolve, the slot falls back to direct equip, which can fail silently for NPC/variant items.

    // Per-character default carrier item-names live in carrier_defaults.hpp::k_carriers[character][slot].itemName.
    // ItemNameTable resolves each to a uint16_t carrier itemId at runtime. This function picks the right row for the
    // active character and falls back to Kliff if the character-specific entry is not catalog-resident.
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

    /**
     * @brief Second-pass tear-down for a DIRECT-applied fake -- one whose carrier collapsed onto the target, or that
     *        had no carrier at all.
     *
     * @details Such a fake renders its per-body rig through the engine's own variant resolver, and the engine needs
     *          the scene-graph tear-down fired TWICE to detach that rig fully. The normal Phase A / Phase B flow only
     *          supplies the second call when a DISTINCT carrier is torn, or when the live real item equals the fake
     *          and Phase B re-tears the same hash. A direct fake with no matching real underneath -- a mask transmog
     *          on a wearer who owns no real mask -- gets a single call, and its rendered rig survives; the symptom is
     *          a fake accessory that keeps showing after a switch to a none-preset.
     * @note No-op when a distinct carrier already handled the tear, when Phase B handles it, or when there is no
     *       fake. SafeTearDown does not mutate the authoritative entry array, so a redundant detach of an
     *       already-gone rig is a safe no-op.
     */
    static void tear_down_direct_fake_second_pass(__int64 a1, std::uint16_t fakeId, std::uint16_t gameTag,
                                                  std::uint16_t liveRealId, bool distinctCarrierTorn) noexcept
    {
        if (fakeId == 0 || distinctCarrierTorn || liveRealId == fakeId)
            return;
        RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), fakeId, gameTag);
        RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), fakeId, gameTag);
    }

    void apply_transmog_with_carrier(__int64 a1, uint16_t carrierId, uint16_t targetId, uint16_t slotSel,
                                     uint16_t excludeTag)
    {
        auto &logger = DMK::Logger::get_instance();

        // The carrier is equipped AS ITSELF; it does not impersonate the target.
        //
        // The prefab-wrapper swap is what delivers the visual: the swap map binds the carrier's own prefab to the
        // target item's prefab, so the engine renders the target mesh while never being told anything untrue about
        // the item. Nothing here copies a descriptor, patches the catalog pointer array, or defeats an equip gate --
        // an item the wearer cannot normally equip needs none of that, because the carrier is always something the
        // wearer CAN equip.
        if (carrierId == 0)
        {
            logger.trace("[carrier] no carrier resolved for target={:#06x}, applying it directly", targetId);
            // Equipped as itself with no swap behind it -- register so the post-apply sweep can find it later.
            PrefabWrapperSwap::register_direct_fake(targetId);
            apply_transmog_core(a1, targetId, slotSel, excludeTag);
            return;
        }

        // Carrier IS the target: the item is equipped as itself and no substitution delivers the visual, so nothing
        // reaches on_struct_copy and the sweep would never learn this slot was filled.
        if (carrierId == targetId)
            PrefabWrapperSwap::register_direct_fake(targetId);

        logger.trace("[carrier] equipping carrier={:#06x} as itself (visual for target={:#06x} comes from the prefab "
                     "swap)",
                     carrierId, targetId);
        apply_transmog_core(a1, carrierId, slotSel, excludeTag);
    }

    /**
     * Refuse an apply whose preset owner and target body disagree.
     *
     * LT picks WHAT to apply from `current_apply_owner()` -- a name held in PresetManager -- and WHERE to apply it
     * from the `a1` it was handed. Nothing tied those two together, so any window where one updates before the other
     * let a character's preset land on a different character's body. Observed twice: a save-load that forces a
     * character switch, and a hot reload while controlling a non-Kliff character, both leaving Kliff wearing the
     * previous character's parts.
     *
     * Only blocks when BOTH sides resolve AND they disagree. A zero host index means the body is not a protagonist at
     * all (a targeted apply onto a companion, which is legitimate) or the actor chain is mid-teardown -- neither is a
     * mismatch, and blocking there would break working paths.
     */
    static bool apply_host_matches_owner(std::uintptr_t a1, const char *site) noexcept
    {
        const auto hostIdx = char_idx_for_equip_slot(a1);
        if (hostIdx == 0)
            return true; // not a protagonist body, or the chain is mid-teardown -- not a mismatch

        // Accept the host if it matches EITHER preset axis.
        //
        // current_apply_owner() alone is not a safe comparand: it returns the pinned EDITING character when pinning
        // is engaged, and the world-entry loop legitimately walks every protagonist in turn with
        // set_active_character() moving underneath it. Comparing against only the owner would block every iteration
        // but one. A body that matches neither axis is the actual defect this guard exists for.
        auto &pm = PresetManager::instance();
        const auto activeIdx = CDCore::character_idx_from_name(pm.active_character());
        const auto editingIdx =
            pm.editing_pinned() ? CDCore::character_idx_from_name(pm.editing_character()) : std::uint32_t{0};
        if (activeIdx == 0 && editingIdx == 0)
            return true; // nothing bound yet -- the caller's own gating decides
        if (hostIdx == activeIdx || hostIdx == editingIdx)
            return true;

        DMK::Logger::get_instance().warning(
            "{}: HOST MISMATCH -- a1 0x{:X} is charIdx {}'s body, but the preset axes are active='{}' ({}) "
            "editing='{}' ({}); skipping so the wrong character is not dressed",
            site, static_cast<std::uint64_t>(a1), hostIdx, pm.active_character(), activeIdx,
            pm.editing_pinned() ? pm.editing_character() : std::string{}, editingIdx);
        return false;
    }

    void apply_single_slot_transmog(__int64 a1, std::size_t slotIdx)
    {
        if (slotIdx >= k_slotCount)
            return;

        auto &logger = DMK::Logger::get_instance();
        auto &mappings = slot_mappings();
        auto &lastIds = last_applied_ids();
        auto &m = mappings[slotIdx];

        // resolve_player_component() walks WorldSystem -> ActorManager -> UserActor -> actor and always returns
        // Kliff's component, whatever character the user controls. An unconditional call clobbers the per-character a1
        // values that the VEC / BatchEquip hooks pass in. Keep it ONLY as a fallback when the passed-in a1 is invalid.
        if (a1 < 0x10000 && world_system_ptr().load(std::memory_order_acquire))
        {
            auto fresh = resolve_player_component();
            if (fresh > 0x10000)
                a1 = fresh;
        }

        if (!apply_host_matches_owner(static_cast<std::uintptr_t>(a1), "apply_single_slot"))
            return;

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

        const uint16_t prevId = lastIds[slotIdx];
        const uint16_t gameTag = static_cast<uint16_t>(k_slotMetadata[slotIdx].gameTag);

        // Compute target: active slot with non-zero id -> transmog, otherwise clear this slot.
        const uint16_t targetId = (m.active && m.targetItemId != 0) ? m.targetItemId : 0;

        // One-shot force flag set by the body-mesh picker when it re-picks a prefab on the same carrier id. The flag
        // bypasses the equality early-out, so Phase A still runs against the real prevId. Phase A then drives the
        // engine's natural-pipeline hook to clean up the prior tgt wrapper. Read-and-clear.
        const bool forceApply = force_apply_pending()[slotIdx];
        if (forceApply)
            force_apply_pending()[slotIdx] = false;

        // Early-out: nothing changed for this slot.
        if (targetId == prevId && targetId != 0 && !forceApply)
        {
            logger.trace("apply_single_slot: slot={} id={:#06x} unchanged", slotIdx, targetId);
            return;
        }

        // --- Scoped dispatch cache clear ---
        // Walk the 24-byte stride cache and zero subCount only for entries whose slotNativeId matches this slot's game
        // tag. This leaves other slots' blobs untouched, so VEC does not re-dispatch them.
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
            return;
        }

        // --- Tear-down scoped to this slot ---
        std::uint16_t realId = 0;
        if (RealPartTearDown::is_ready())
        {
            realId = RealPartTearDown::get_real_item_id(reinterpret_cast<void *>(a1), gameTag);

            // Phase A: tear down previous fake. Runs even when the previous fake itemId matches the live real item.
            // Fake and real get the same treatment, so the tear-down/apply sequence is always complete.
            if (prevId != 0)
            {
                const auto prevCarrier = last_applied_carrier_ids()[slotIdx];
                if (prevCarrier != 0 && prevCarrier != static_cast<uint16_t>(prevId))
                {
                    RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), prevCarrier, gameTag);
                }
                RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), static_cast<uint16_t>(prevId),
                                                       gameTag);

                // Direct-applied fake with no matching real underneath: it needs a second detach.
                // See tear_down_direct_fake_second_pass.
                tear_down_direct_fake_second_pass(a1, static_cast<std::uint16_t>(prevId), gameTag, realId,
                                                  prevCarrier != 0 &&
                                                      prevCarrier != static_cast<std::uint16_t>(prevId));
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
            // Arm the prefab-wrapper swap for this install.
            //
            // apply_all_transmog is the only caller of notify_apply_starting, and it calls it past an early-out. That
            // early-out skips the rest of the function when neither the preset nor the live gear changed. A
            // single-slot apply therefore reaches the engine with whatever swap state the last full apply left
            // behind. That state can belong to a different character's wrappers. The natpipe hook then matches
            // nothing and the slot renders its base mesh in place of the swapped prefab.
            //
            // Arm on the INSTALL direction only. The tear-down branch below must leave the swap state alone. An armed
            // map rewrites the wrapper the engine's unlink pass looks for, the unlink misses, and the old mesh stays
            // painted. That is also why this is not a plain notify_apply_starting call. That path reverts every live
            // substitution across all slots, and only this one slot re-installs.
            // Park the target being replaced BEFORE arming, so the sweep below can tell it apart from what this
            // apply re-installs. The full path gets this from the deactivate cycle inside notify_apply_starting;
            // scoping it to this one slot is what keeps the other slots' live targets off the victim list.
            if (prevId != 0 && prevId != targetId)
                PrefabWrapperSwap::park_slot_target_for_sweep(prevId);

            PrefabWrapperSwap::ensure_armed_for_slot_apply();

            const auto tmSlot = static_cast<TransmogSlot>(slotIdx);
            // Use current_apply_owner so a targeted-apply onto a non-controlled body resolves its carrier from THAT
            // body's defaults. PresetManager::active_character() returns the controlled character, which under
            // pin+flag is the wrong axis. A carrier mismatch installs the wrong wrapper family on the body and
            // produces visual cross-talk through the swap map.
            const auto &activeChar = current_apply_owner();
            // Every slot goes through a carrier now. The carrier supplies a legitimately equippable item; the prefab
            // swap supplies the visual. There is no longer a "can the wearer equip the target" question to branch on.
            const uint16_t carrierId = default_carrier_for_slot(tmSlot, activeChar);
            const bool useCarrier = carrierId != 0;

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
                // empty engine source records therefore render colorless. To color those, the user must seed preset
                // dye through Capture Outfit or the per-slot "Sync from live" button.
                DyeRecordInject::clear_slot_dye_state();
            }

            ColorOverride::SetterSubstitute::set_active_slot(static_cast<int>(slotIdx));
            // Notify ColorOverride of the user-INTENDED target item for this slot. This wipes the swatch table only
            // when the user's chosen transmog target ACTUALLY changes. It does not wipe when the resolved target flips
            // to the carrier for the duration of an untick, which the dispatch path does.
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
                apply_transmog_with_carrier(a1, carrierId, targetId,
                                            slot_needs_explicit_destination(tmSlot)
                                                ? static_cast<uint16_t>(game_slot_from_transmog(tmSlot))
                                                : k_noGameTag,
                                            paired_first_half_tag(tmSlot));
            }
            else
            {
                logger.debug("apply_single_slot: slot={} target={:#06x}", slot_name(tmSlot), targetId);
                apply_transmog(a1, targetId);
            }

            DyeRecordInject::clear_slot_dye_state();

            // Detach the replaced target now that the new one is installed. Mirrors notify_apply_finished, which
            // the single-slot path does not call.
            PrefabWrapperSwap::sweep_after_slot_apply();

            // Rebuild through refresh_slot_appearance, NOT the bare refresh_slot_visual.
            //
            // The rebuild drives a DyeCopier call, and the dye state was cleared just above -- so a bare rebuild
            // re-emits the slot with the engine's natural records and the transmog loses its colour.
            // refresh_slot_appearance republishes this slot's dye (and its ColorOverride slot) around the rebuild,
            // which is what the injector's detour consumes.
            //
            // Mirrors the loop apply_all_transmog runs after notify_apply_finished.
            //
            // Every slot installs through a carrier, and the carrier for a given slot does not change when the user
            // picks a different target -- only the swap map does. So the equip layer sees the same item go back on,
            // has nothing to reconcile, and leaves the previously realized mesh exactly where it is. Erasing the old
            // claim does not retract it either; the engine only reconciles on a rebuild.
            //
            // Without this the full-apply path cleared a target change and the single-slot path did not, which is
            // what made a pick made with Instant Apply keep showing the previous item.
            if (prevId != targetId)
                refresh_slot_appearance(slotIdx);

            lastIds[slotIdx] = targetId;
            last_applied_carrier_ids()[slotIdx] = (useCarrier && carrierId != 0) ? carrierId : 0;
            // Phase B set real_damaged when it tore down the real item for this slot. The fake is applied now, so
            // clear the flag and keep apply_all_transmog from seeing stale damage state on later cycles.
            real_damaged()[slotIdx] = false;
        }
        else
        {
            // Clearing this slot. Two cases:
            //  - active + none (checkbox ticked, picker = none): user wants to show an EMPTY slot (bare head, etc.).
            //    Do NOT restore the real item. It is already gone: Phase B of the call that INSTALLED the fake tore
            //    it down, and nothing has put it back since. This call's Phase B is gated on `targetId != 0` and does
            //    not run, so nothing here removes it -- leaving it alone is what keeps the slot empty.
            //  - inactive (!m.active): LT controlled the slot before, so restore the real item and it reappears.
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

    }

    /**
     * Report where the engine would place each enabled slot's carrier.
     *
     * SlotPopulator resolves the item to a slot before doing anything and refuses outright on 0xFFFF, so a carrier
     * that does not resolve equips nothing and the slot silently stays as it was. Worth surfacing: the failure is
     * otherwise invisible.
     *
     * Both halves of a paired slot resolve to the FIRST slot's tag -- they share one equip type -- which is why the
     * second half needs its destination named explicitly.
     */
    static void log_carrier_resolution(__int64 a1, const std::string &charName)
    {
        const auto fn = item_to_slot_resolve_fn();
        if (!fn)
            return;
        std::string line;
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            const auto sl = static_cast<TransmogSlot>(i);
            if (!slot_enabled(sl))
                continue;
            const auto carrier = default_carrier_for_slot(sl, charName);
            if (carrier == 0)
                continue;
            const auto placed = item_to_slot_seh(fn, a1, carrier);
            if (!line.empty())
                line += ", ";
            line += std::format("{}:{:#06x}->{}", slot_name(sl), carrier,
                                placed == 0xFFFF ? std::string{"REFUSED"} : std::format("{:#06x}", placed));
        }
        DMK::Logger::get_instance().trace("[dispatch] carrier resolution [{}]", line);
    }

    void apply_all_transmog(__int64 a1)
    {
        auto &logger = DMK::Logger::get_instance();
        // Local copy of slot_mappings. It carries a synthesized "all-slots-cleared" view when the user toggles LT off,
        // and it leaves the persisted preset state untouched. See the flag_enabled() block immediately below.
        auto mappings = slot_mappings();
        auto &lastIds = last_applied_ids();

        // An Enabled=off toggle must NOT early-out of the dispatcher. An early return freezes the cleanup pass (the
        // k_tearDownSlots loop with mappings[idx].active==false + realItemId==0 + last_applied_real_ids[idx]!=0), and
        // stale restore meshes from a prior LT apply then leak through the next organic radial unequip. Instead, force
        // every mapping inactive in this local copy and let the dispatcher run as if the user had unticked every slot.
        // The cleanup pass keeps tearing down stale fakes. The apply pass writes nothing, because every slot has
        // active==false.
        //
        // Nothing schedules this pass on an equip any more: the BatchEquip and VisualEquipChange hooks are gone, so
        // while LT is disabled it runs only from the UI, a hotkey, or world entry. SocketMeshOverride installs
        // nothing while disabled either, so no new fakes appear in the meantime.
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

        if (!apply_host_matches_owner(static_cast<std::uintptr_t>(a1), "apply_all_transmog"))
            return;

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
        // per-actor sub-handler at CCC+0x130 is wired. If the dispatcher runs against an actor whose sub-handler is
        // still null, the engine path under tear_down / tear_down_fake dereferences it as `v15 = *(v7 + 304); *v15;`.
        // The per-slot SEH wrappers then burst-catch the faults, and every slot exits early. The carrier apply
        // continues against a half-wired actor before it raises out of the outer __try. The gate at this entry point
        // covers every caller (manual_apply, the multi-protagonist worker path, any future trigger) and does not
        // couple them to LT-specific readiness semantics.
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

        logger.trace("[dispatch] apply_all_transmog entry a1={:#018x}", static_cast<uint64_t>(a1));

        log_carrier_resolution(a1, PresetManager::instance().active_character());

        // Snapshot lastIds for diagnostic logging.
        const std::array<uint16_t, k_slotCount> prevIds = lastIds;

        // One-shot per-slot "force apply" snapshot (read-and-clear). Set by the body-mesh picker when the user re-picks
        // a prefab on the same carrier id (the id is unchanged, but the src->tgt wrapper map differs). Without this
        // signal the dispatcher sees `wouldBe == prevIds[i]` and skips both presetChanged AND slotNeedsWork. Phase A
        // `tear_down_fake` then never runs for the slot, and nothing drives the engine's natural-pipeline hook to
        // clean up the prior tgt wrapper. With the flag set, the dispatcher behaves as if the slot's preset changed,
        // and prevIds[i] stays intact so Phase A still tears down the prior carrier.
        std::array<bool, k_slotCount> forceApply{};
        {
            auto &fa = force_apply_pending();
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                forceApply[i] = fa[i];
                fa[i] = false;
            }
        }

        // Early-out: skip all work if neither the preset nor the real armor changed since the last successful
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
            // nothing else changed. The game can re-equip real items after the initial tear-down during the load
            // sequence.
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
                // Trivially-destructible char buffers -- std::string here violates SEH/object-unwinding rules, because
                // the function contains __try frames. 8 chars per slot ("0x1234,") x 20 slots = 160 + slack.
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

            // Per-slot "needs work" flags. Computed BEFORE last_applied_real_ids is overwritten, so the comparison
            // runs against the previous state. A slot needs tear-down + re-apply if its preset target changed OR its
            // underlying real item changed. Unchanged slots are skipped -- no tear-down, no cache clear, no
            // SlotPopulator call -- so they do not flicker.
            //
            // Unticked slots whose real changes ARE still marked: a prior restore through SlotPopulator can leave a
            // dispatch cache entry and a scene-graph mesh. The cleanup pass after the untick-restore loop relies on
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
                // changes -- an earlier restore through SlotPopulator can leave a dispatch entry that the game's own
                // unequip flow cannot remove. liveRealIds and last_applied_real_ids are both indexed by TransmogSlot
                // (k_slotCount-wide), so the comparison is direct.
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

            // NOTE: last_applied_real_ids is updated at the END of the function, after all applies succeed. On a
            // mid-apply fault (e.g. reload SEH) the old values remain, so the next retry detects the real-armor change
            // and tries again.
        }

        // Clear lastIds for slots without a new target.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            auto &m = mappings[i];
            if (!m.active || m.targetItemId == 0)
                lastIds[i] = 0;
        }

        // SlotPopulator maintains a dispatch cache on the component at (basePtr, count, cap). The k_compSlotCache*
        // constants at the top of this file carry the triple's offsets. Each entry is 24 bytes:
        //   +0x00 uint16  slotNativeId
        //   +0x08 __int128* subArray (queued ItemInfoBlobs)
        //   +0x10 uint32  subCount   (append-only, NEVER reset by game)
        //   +0x14 uint32  subCap
        //
        // Two hazards:
        //  (1) Stale entries survive across presets. A save/restore of the
        //      count re-exposes previous-preset entries. SlotPopulator's
        //      linear search then finds an old slotNativeId and *appends*
        //      a new blob to its existing subArray. The stale blob is
        //      still dispatched to VEC, which is the stale helm bug.
        //  (2) Even on a slot this pass DOES re-populate, the game never
        //      clears subCount, so the previously queued blob lingers and
        //      is replayed alongside the new one.
        //
        // Fix: only clear subCount for dispatch cache entries whose slotNativeId matches a slot this pass re-applies.
        // A blanket clear nukes unchanged slots' blobs, which forces the game to re-dispatch them through VEC and
        // makes unchanged gear flicker.
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
            return;
        }

        // Two-phase scene-graph tear-down before applying fakes.
        //
        // Phase A -- tear down the previous preset's fake meshes, with lastIds[] as the itemId source.
        // Phase B -- tear down the REAL item in the auth table for every active slot that needs work, even when the
        //   new fake equals the real item.
        //
        // Both phases go through the engine's part-detach entry point, which detaches particle emitters and anim
        // controllers from the scene graph. The auth table is NOT mutated.
        //
        // Game slot tags, read with a hardware breakpoint on the engine's slot dispatcher:
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

            // Phase A: previous fakes, taken from lastIds as it stood before this apply.
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
                    // First-claim hide: active-none slot LT never owned (prevIds==0, no carrier history). Phase B
                    // alone calls the scene-graph tear-down once, and one call does not detach the part for a slot
                    // where LT never placed a carrier (e.g. Mask/Necklace on the first apply of an all-none preset).
                    // The doubled call here matches the working manual path (transmog-something -> none), which fires
                    // Phase A on the prior carrier plus Phase B on the real entry -- same hash, same slot tag, twice.
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

                // Phase A runs unconditionally: fake and real get equal treatment, so a previous fake that matches the
                // live real item is still torn down.
                if (prevCarrier != 0 && prevCarrier != static_cast<std::uint16_t>(prevId))
                {
                    logger.trace("[dispatch] tear_down_fake slot={:#06x} "
                                 "carrier={:#06x} (then target={:#06x})",
                                 td.gameTag, prevCarrier, static_cast<std::uint16_t>(prevId));
                    RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), prevCarrier, td.gameTag);
                }
                RealPartTearDown::tear_down_by_item_id(reinterpret_cast<void *>(a1), static_cast<std::uint16_t>(prevId),
                                                       td.gameTag);

                // Direct-applied fake (Mask/Necklace, or any carrier==target collapse) with no matching real
                // underneath: give it the extra detach its rendered per-body rig needs to come off. No-op for
                // distinct-carrier items and when Phase B re-tears the same hash (live real == fake). See
                // tear_down_direct_fake_second_pass.
                tear_down_direct_fake_second_pass(a1, static_cast<std::uint16_t>(prevId),
                                                  static_cast<std::uint16_t>(td.gameTag), realItemId[k],
                                                  prevCarrier != 0 &&
                                                      prevCarrier != static_cast<std::uint16_t>(prevId));
            }

            // Phase B: real items for any active slot. Runs unconditionally -- fake and real get equal treatment, so
            // a new fake that matches the live real still tears down the real part. Only slots without a slotNeedsWork
            // flag, and inactive slots, are skipped.
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
        // from the gear active at the last swap apply, deactivate so the upcoming apply loop's substitutions do not
        // re-bind target wrappers to the new gear. (The reverse-write of prior tracked structs already ran
        // pre-tear-down, so the engine can unlink them during tear-down.) Order: Helm/Chest/Cloak/Gloves/Boots -- the
        // fixed 5-armor order of the prefab-wrapper-swap notify_apply_starting contract.
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
                // Unchanged slot: preserve its lastIds entry but do not re-apply. If it has a live dispatch cache
                // entry, the game keeps rendering it.
                if (m.active && m.targetItemId != 0)
                    lastIds[i] = m.targetItemId;
                continue;
            }
            if (!m.active || m.targetItemId == 0)
                continue;

            // Fake and real get equal treatment: SlotPopulator runs unconditionally, even when the new fake itemId
            // matches the intact live real item. Do NOT add a skip-on-match fast path. The apply sequence must stay
            // one predictable path, and the CPU it saves does not pay for the divergence between matched and
            // unmatched re-applies.
            //
            // Decide: direct apply or carrier-assisted apply. current_apply_owner picks the editing character when the
            // dropdown pin is engaged, so a targeted-apply on a non-controlled body installs THAT body's carrier
            // family. See the matching block in apply_single_slot_transmog for the cross-talk failure mode this
            // avoids.
            const auto tmSlot = static_cast<TransmogSlot>(i);
            const auto targetId = m.targetItemId;
            const auto &activeChar = current_apply_owner();
            // Every slot goes through a carrier now. The carrier supplies a legitimately equippable item; the prefab
            // swap supplies the visual. There is no longer a "can the wearer equip the target" question to branch on.
            const uint16_t carrierId = default_carrier_for_slot(tmSlot, activeChar);
            const bool useCarrier = carrierId != 0;

            // Feed the active preset's per-slot dye state into the record injector. Its inline detour on the engine's
            // dye-publish function appends fabricated ARMOR_MOD records to the publish vector post-trampoline. The
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
            // 0) so an untick does not trigger a target-change wipe of seeded placeholders.
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
                apply_transmog_with_carrier(a1, carrierId, targetId,
                                            slot_needs_explicit_destination(tmSlot)
                                                ? static_cast<uint16_t>(game_slot_from_transmog(tmSlot))
                                                : k_noGameTag,
                                            paired_first_half_tag(tmSlot));
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
            lastIds[i] = m.targetItemId;
            last_applied_carrier_ids()[i] = (useCarrier && carrierId != 0) ? carrierId : 0;
            ++ourWrittenCount;
        }

        // When a slot's checkbox is UNTICKED (!m.active), LT controlled the slot before and Phase B tore down the real
        // item. Restore the real item so it reappears. "Active + none" (checkbox ticked, dropdown=none) means "show
        // empty" -- do NOT restore.
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
                    // Do NOT mirror this live dye into the active preset here. The `!m.active` gate above is shared
                    // with mass-reset paths the user never triggers directly (mod-disable at the top of this same
                    // function, character switch, Unpin, preset load, ColorOverride re-init). A mirror here silently
                    // bakes the real item's dye into the preset on every toggle-off and every character switch.
                    // Real-dye capture into the preset stays explicit: Capture Outfit (mass), or the per-slot "Sync
                    // from live" button in the dye popup.
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
                    // The real item was unequipped (realId=0), but an earlier restore through SlotPopulator left a
                    // scene-graph mesh entry. Tear it down so the visual disappears. The old real ID is still in
                    // last_applied_real_ids, which this function overwrites only at its end.
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

        // Cleanup pass: tear down stale scene-graph meshes left by a prior restore through SlotPopulator. This handles
        // the case where apply_single_slot restored the real item and created a scene-graph entry, and the user then
        // unequipped it in the game inventory. The untick-restore block above does not catch that case, because
        // apply_single_slot already cleared prevIds and real_damaged. The signature detected here is slotNeedsWork +
        // unticked + real=0 + an old real in last_applied_real_ids, which is not overwritten yet.
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

        // Suppression rule: suppress every active slot whose real item is torn down, or will be. If the fake itemId
        // matches the real equipped id, the real item is still live and LT must NOT suppress it.
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
            // Trivially-destructible char buffers -- std::string here violates SEH/object-unwinding rules, because the
            // function contains __try frames. Split prev / now across two log lines so each fits a normal terminal.
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

        // Commit the real-armor snapshot AFTER all applies succeed. On a fault (SEH during reload) this line is never
        // reached, and the next retry detects the real-armor change correctly.
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

            // Rebuild the slots that REPLACED an earlier LT target.
            //
            // The sweep above erases the previous target's CLAIM, and the claim count does drop -- but the part is
            // already realized, and dropping a claim does not retract what is on screen. The engine only reconciles
            // on a rebuild, so the stale mesh survives until one happens.
            //
            // This is what lets a target change stay instant: the new visual installs, the stale claim is erased,
            // and the slot rebuilds against the claims that remain -- no tear-down anywhere in the path.
            //
            // Two conditions, and BOTH matter:
            //
            // - `prevIds[i] != 0` -- there has to be an earlier LT target to retract. On the first apply after a
            //   world load the slot held the REAL item, which Phase A/B already tore down, so there is nothing stale
            //   and the rebuild would be pure cost.
            // - `prevIds[i] != targetItemId` -- measured against the entry snapshot, NOT against `lastIds`. That IS
            //   last_applied_ids(), and the apply loop above has already written this apply's targets into it, so
            //   every slot would compare equal and nothing would ever rebuild.
            //
            // The rebuild goes through refresh_slot_appearance, NEVER the bare refresh_slot_visual. The apply loop
            // clears the dye state after each slot, so a bare rebuild drives a DyeCopier call with nothing published
            // and the engine re-emits its natural records -- which silently strips the colour the apply just
            // injected. refresh_slot_appearance republishes this slot's dye and rebinds its ColorOverride slot around
            // the rebuild. Same reasoning as the single-slot path; see its call site.
            for (std::size_t i = 0; i < k_slotCount; ++i)
            {
                const auto sl = static_cast<TransmogSlot>(i);
                if (!slot_enabled(sl) || !mappings[i].active || mappings[i].targetItemId == 0)
                    continue;
                if (prevIds[i] == 0 || prevIds[i] == mappings[i].targetItemId)
                    continue; // no earlier target, or unchanged -- nothing stale to reconcile
                refresh_slot_appearance(i);
            }
        }

        // PrefabWrapperSwap stays active across applies. Do NOT schedule an auto-deactivate after each apply. The
        // wrapper-substitution path has no cheap teardown -- a heap walk on deactivate stalls preset switches by about
        // a minute. The residual helm leak also sits outside that path: it lives in scene-graph children that the
        // engine re-parents through its runtime-resource-pointer keying. Clearing that residue needs PAZ-level
        // patching. Users press LT's Clear button when they want the swap torn down explicitly.
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

        // Snapshot the previously applied fakes BEFORE clearing lastIds. Iteration order across `k_slotMetadata` is
        // irrelevant for correctness: prevFakeId / prevCarrierId are indexed by `k` (the array slot), and the per-slot
        // snapshot reads `lastIds` by `slot` (the TransmogSlot enum value). The engine-only tag 0x0015 is absent from
        // `k_slotMetadata` by design (see `TransmogSlot` enum in `shared_state.hpp`), so the loop skips it
        // automatically.
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

                // Tear the carrier's own identity down first when it is a distinct item.
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

                // Direct-applied orphan fake with no matching real underneath: it needs a second detach.
                // See tear_down_direct_fake_second_pass.
                tear_down_direct_fake_second_pass(a1, fakeId, gameTag, realId, cId != 0 && cId != fakeId);
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
            auto entryDesc = *reinterpret_cast<uintptr_t *>(a1 + AuthTable::k_containerPtrOffset);
            if (entryDesc > 0x10000)
            {
                auto entryArray = *reinterpret_cast<uintptr_t *>(entryDesc + AuthTable::k_containerArrayBaseOffset);
                auto entryCount = *reinterpret_cast<uint32_t *>(entryDesc + AuthTable::k_containerCountOffset);

                auto savedCount = *reinterpret_cast<uint32_t *>(a1 + k_compSlotCacheCountOffset);
                *reinterpret_cast<uint32_t *>(a1 + k_compSlotCacheCountOffset) = 0;

                for (uint32_t e = 0; e < entryCount && entryArray > 0x10000; ++e)
                {
                    auto base = entryArray + e * AuthTable::k_entryStride;
                    auto gameSlot = *reinterpret_cast<int16_t *>(base + AuthTable::k_entrySlotTagOffset);
                    auto itemId = *reinterpret_cast<uint16_t *>(base + AuthTable::k_entryItemIdOffset);

                    if (itemId == 0 || itemId == 0xFFFF)
                        continue;

                    auto tmSlot = slot_from_game_slot(gameSlot);
                    if (!tmSlot.has_value())
                        continue;

                    // Snapshot the equipped item's live dye records and re-publish them through the inject channel, so
                    // the following apply_transmog -> SlotPopulator -> DyeCopier round-trip emits them into the render
                    // struct's dst+120. Without this, the synthesized swapEntry passes through DyeCopier empty. The
                    // engine then resolves the slot to its factory palette and paints toggled-off items un-dyed.
                    DyeRecordInject::ChannelState liveDye[DyeRecordInject::k_dyeChannelCount];
                    if (DyeRecordInject::read_entry_dye_records(base, liveDye) > 0)
                    {
                        DyeRecordInject::log_dye_snapshot("restore", slot_name(*tmSlot), liveDye);
                        // sparse: mirror the source channels exactly, so the pass does not paint mesh parts that the
                        // real item never colored.
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
        // The natpipe hook must stay armed (s_active=true with s_swapMapPerChar populated) so any later organic
        // unequip or scene-graph teardown -- for example when the user swaps gear through the radial after a Clear --
        // can still find and unlink the Bastier-target wrappers LT installed in parent+88. If the hook is disarmed
        // here, the engine searches with Kliff src wrappers, misses the Bastier targets, and leaks ghost meshes. That
        // ghost-helm leak is the reason this hook exists.
        //
        // The on_struct_copy hook is independently silenced after toggle-off by its in_transmog() gate, so leaving the
        // swap map armed only matters during the engine's own cleanup walks, which is exactly when it must fire.

        logger.info("Transmog CLEAR: done");
    }

} // namespace Transmog
