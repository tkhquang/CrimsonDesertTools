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

#include <DetourModKit/address.hpp>
#include <DetourModKit/error.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace Transmog
{
    // Structural plausibility screen for anything treated as a live pointer.
    //
    // The engine hands component pointers through `__int64` parameters, so a negative (garbage) value must fail. The
    // cast back to std::uintptr_t is bit-preserving, and the canonical upper bound in memory::is_plausible_ptr is what
    // rejects the widened negative that a bare floor lets through.
    static bool plausible_engine_ptr(__int64 value) noexcept
    {
        return DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(value)});
    }

    // Candidate-exclusion list on the equip-slot component. The item -> slot resolver walks the character's candidate
    // slots and returns the first that validates, and the validator rejects any candidate found in the WORD array at
    // `COMP_SLOT_EXCLUDE_LIST_OFFSET` (count at `COMP_SLOT_EXCLUDE_COUNT_OFFSET`). The pair is empty in normal play,
    // which is what makes it safe to borrow for one equip. It sits at its own depth in the component: re-derive it from
    // the validator's own live read of the array and count, never by scaling another field's shift.
    constexpr std::ptrdiff_t COMP_SLOT_EXCLUDE_LIST_OFFSET = 104;
    constexpr std::ptrdiff_t COMP_SLOT_EXCLUDE_COUNT_OFFSET = 112;

    // SlotPopulator maintains a dispatch cache on the component at (basePtr, count, cap). The three constants are one
    // field triple and must move TOGETHER: base+0 is the pointer, base+8 the count, base+0xC the capacity.
    //
    // Nothing else in the mod moves with them. The exclusion pair above, the auth-table container pointer in
    // auth_table.hpp and this triple all sit at different depths in the component and drift by different amounts on
    // the same patch, so each has to be re-derived from its own live instruction. Scaling one from another's delta
    // lands on a plausible-looking neighbor and fails silently. Verify against the live SlotPopulator body on patch
    // day. That body forms one base pointer and reads the other two members off it:
    //   lea  r15, [a1+COMP_SLOT_CACHE_BASE_PTR_OFFSET]
    //   mov  r8d, [r15+0x08]     ; count
    //   mov  r9,  [r15]          ; basePtr
    // The later grow check pins the capacity directly instead of by adjacency:
    //   mov ecx,[r15+0x0C] ; mov eax,[r15+0x08] ; cmp ecx,eax ; ja
    //
    // A stale triple fails SILENTLY in two directions. If the count offset lands on a neighboring u32 that is always
    // 0, apply looks like a no-op. If it lands on the LOW 32 bits of the basePtr qword, the read yields a wildly
    // inflated count - the low bits of a heap address. A write at that offset then shreds the dispatch-cache pointer.
    // Writes at stale offsets also scribble into adjacent fields and corrupt the component one slot at a time.
    //
    // Check the triple against a live component rather than against the disassembly alone: a correct base reads a
    // populated (basePtr, count, cap), while a stale one reads 0/0/0 and surfaces as `post-apply live_count=0` on
    // every apply.
    constexpr std::ptrdiff_t COMP_SLOT_CACHE_BASE_PTR_OFFSET = 0x1F8;
    constexpr std::ptrdiff_t COMP_SLOT_CACHE_COUNT_OFFSET = 0x200;
    constexpr std::ptrdiff_t COMP_SLOT_CACHE_CAP_OFFSET = 0x204;

    // Auth-table geometry (container pointer, entry stride, field offsets) lives in auth_table.hpp - one copy for the
    // whole mod, because the whole struct moves as a unit on patch day.

    // (TransmogSlot, engine slot tag) pairs the dispatcher iterates for tear-down + the auth-table real-id snapshot.
    // Sourced from slot_metadata.hpp's single per-slot table. The local TearDownSlot alias keeps existing call sites
    // (`td.slot`, `td.game_tag`) reading unchanged. Order matches the TransmogSlot enum.
    using TearDownSlot = SlotMetadata;
    static constexpr auto &TEAR_DOWN_SLOTS = SLOT_METADATA;
    static constexpr std::size_t TEAR_DOWN_COUNT = SLOT_COUNT;

    // Walk the auth-table for the entry whose +0xC8 slot_tag matches `game_tag`, snapshot its dye-record vector, and
    // publish through dye_record_inject so the next apply_transmog -> SlotPopulator -> DyeCopier round-trip emits
    // exactly those records into the render struct's dst+120. Returns true when it published records. The caller must
    // call clear_slot_dye_state after the apply pass.
    //
    // Used ONLY by the untick-restore branch in apply_all_transmog (`!m.active && prev_ids != 0`). When the user
    // unticks a slot and the real item returns to view, this repaints it in its current inventory dye instead of its
    // factory palette.
    //
    // Fakes with no explicit preset dye flow through with their natural engine records. Monster-carrier fakes whose
    // engine source is empty render colorless. To seed preset dye for those fakes the user must call Capture Outfit
    // (mass) or the per-slot "Sync from live" button in the dye popup, which are the only paths that mutate the active
    // preset.
    static bool publish_entry_dye_for_gameslot(__int64 a1, std::int16_t game_tag) noexcept
    {
        uintptr_t entry_base = 0;
        __try
        {
            const auto entry_desc = *reinterpret_cast<uintptr_t *>(a1 + auth_table::CONTAINER_PTR_OFFSET);
            if (!plausible_engine_ptr(static_cast<__int64>(entry_desc)))
                return false;
            const auto entry_array =
                *reinterpret_cast<uintptr_t *>(entry_desc + auth_table::CONTAINER_ARRAY_BASE_OFFSET);
            const auto entryCount = *reinterpret_cast<uint32_t *>(entry_desc + auth_table::CONTAINER_COUNT_OFFSET);
            for (uint32_t e = 0; e < entryCount && plausible_engine_ptr(static_cast<__int64>(entry_array)); ++e)
            {
                const auto base = entry_array + e * auth_table::ENTRY_STRIDE;
                const auto sl = *reinterpret_cast<int16_t *>(base + auth_table::ENTRY_SLOT_TAG_OFFSET);
                if (sl == game_tag)
                {
                    entry_base = base;
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (entry_base == 0)
            return false;
        dye_record_inject::ChannelState live[dye_record_inject::DYE_CHANNEL_COUNT];
        if (dye_record_inject::read_entry_dye_records(entry_base, live) == 0)
            return false;
        const auto tm_slot = slot_from_game_tag(game_tag);
        dye_record_inject::log_dye_snapshot("restore", tm_slot.has_value() ? slot_name(*tm_slot) : "?", live);
        // Sparse mode: emit only the channels that the auth-table entry carried. Dense fill uses the first active
        // channel as a fallback, so it paints mesh sub-parts (e.g. cloak facings) that the real item never colored.
        dye_record_inject::set_slot_dye_state(live, /*sparse=*/true);
        return true;
    }

    /**
     * @brief Guarded read of the actor pointer the equip component carries at `a1+8`.
     * @param a1 The equip-slot component the apply runs against.
     * @return The actor pointer, or ErrorCode::ReadFaulted when the component wrapper no longer reads.
     * @details Every apply entry point probes this before it touches the component. A wrapper captured by a hook
     *          before a world reload faults here or yields garbage, and the caller falls back to the WorldSystem walk.
     */
    static DMK::Result<std::uintptr_t> read_component_actor(__int64 a1) noexcept
    {
        return DMK::memory::read<std::uintptr_t>(DMK::Address{static_cast<std::uintptr_t>(a1)}.offset(8));
    }

    /**
     * @brief Renders a per-slot id array as "0x0000,0x0001,..." into @p out.
     * @param ids SLOT_COUNT item ids.
     * @param out Destination buffer. It always ends NUL-terminated.
     * @param cap Size of @p out. 8 chars per slot plus slack is enough.
     * @details The callers hold __try frames, where MSVC forbids an object that needs unwinding, so the log lines
     *          build in trivially-destructible char buffers rather than in a std::string.
     */
    static void format_slot_ids(const std::uint16_t *ids, char *out, std::size_t cap) noexcept
    {
        if (out == nullptr || cap == 0)
            return;
        out[0] = '\0';
        if (ids == nullptr)
            return;
        std::size_t off = 0;
        for (std::size_t i = 0; i < SLOT_COUNT && off + 1 < cap; ++i)
        {
            const int n = std::snprintf(out + off, cap - off, "%s0x%04x", i ? "," : "", static_cast<unsigned>(ids[i]));
            if (n > 0)
                off += static_cast<std::size_t>(n);
        }
        out[cap - 1] = '\0';
    }

    /**
     * @brief Publishes the active preset's dye state for one slot, or clears the published state.
     * @param slot_idx Slot the next engine call rebuilds.
     * @details The injector's post-trampoline detour consumes the published state on the next DyeCopier call. With no
     *          active channel for the slot, the clear makes that detour skip injection, so DyeCopier's natural copy
     *          of the item's own records wins. A monster-carrier fake whose engine source records are empty then
     *          renders colorless, and the user seeds preset dye for it through Capture Outfit or the per-slot "Sync
     *          from live" button.
     */
    static void publish_preset_dye_for_slot(std::size_t slot_idx)
    {
        static_assert(
            Transmog::DYE_CHANNEL_COUNT == dye_record_inject::DYE_CHANNEL_COUNT,
            "channel-count mismatch between preset model and dye injector"
        );
        const Preset *active_preset = PresetManager::instance().active_preset();
        const SlotDyeChannels *slot_dye =
            (active_preset && slot_idx < active_preset->slots.size()) ? &active_preset->slots[slot_idx].dye : nullptr;
        if (slot_dye == nullptr || !any_dye_active(*slot_dye))
        {
            dye_record_inject::clear_slot_dye_state();
            return;
        }

        dye_record_inject::ChannelState state[dye_record_inject::DYE_CHANNEL_COUNT];
        for (std::size_t k = 0; k < dye_record_inject::DYE_CHANNEL_COUNT; ++k)
        {
            const auto &ch = (*slot_dye)[k];
            state[k] = {ch.group_hash, ch.r, ch.g, ch.b, ch.material_id, ch.repair_byte};
        }
        dye_record_inject::set_slot_dye_state(state, active_preset->slots[slot_idx].dye_sparse);
    }

    // The engine calls below live in POD-only wrappers because MSVC forbids `__try` in a frame that needs object
    // unwinding, and the apply path formats log strings. Each returns a failure value rather than letting a fault
    // escape: an unguarded fault here aborts the whole apply, so one bad slot takes every other slot with it.

    /// POD-only SEH wrapper: asks the engine which slot takes `item_id`, or `NO_GAME_TAG` when none does.
    static std::uint16_t item_to_slot_seh(ItemToSlotResolveFn fn, std::int64_t a1, std::uint16_t item_id) noexcept
    {
        if (!fn)
            return NO_GAME_TAG;
        __try
        {
            return static_cast<std::uint16_t>(fn(a1, static_cast<std::int16_t>(item_id)) & 0xFFFF);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return NO_GAME_TAG;
        }
    }

    /**
     * @brief Temporarily excludes a slot from the engine's item -> slot resolution.
     * @param a1 The live equip-slot component.
     * @param buf Caller-owned one-entry exclusion array. It must outlive the equip.
     * @param exclude_tag Engine slot tag to hide from the resolver.
     * @param saved_ptr Receives the list pointer the component held.
     * @param saved_count Receives the count the component held.
     * @return false when the list is already populated, so a live exclusion set is never displaced, and false when a
     *         read or a store fails.
     * @details The engine's item -> slot resolver walks the character's candidate slots and returns the FIRST that
     *          validates. The validator rejects any candidate listed in the exclusion array (see
     *          COMP_SLOT_EXCLUDE_LIST_OFFSET), and that array is empty in normal play. An exclusion of the first
     *          half of a pair for the duration of one equip makes the resolver fall through to the second,
     *          otherwise unreachable half, because both halves share one item type and the first always
     *          wins.
     *
     *          It touches no equip state. This is a transient resolution filter, not the auth table.
     */
    static bool arm_slot_exclusion(
        std::int64_t a1,
        std::uint16_t *buf,
        std::uint16_t exclude_tag,
        std::uint64_t &saved_ptr,
        std::uint32_t &saved_count
    ) noexcept
    {
        if (!plausible_engine_ptr(a1) || !buf || exclude_tag == NO_GAME_TAG)
            return false;

        const DMK::Address list_field{static_cast<std::uintptr_t>(a1 + COMP_SLOT_EXCLUDE_LIST_OFFSET)};
        const DMK::Address count_field{static_cast<std::uintptr_t>(a1 + COMP_SLOT_EXCLUDE_COUNT_OFFSET)};
        const auto live_ptr = DMK::memory::read<std::uint64_t>(list_field);
        const auto live_count = DMK::memory::read<std::uint32_t>(count_field);
        if (!live_ptr || !live_count)
            return false;
        if (*live_count != 0)
            return false; // something already uses it - do not displace
        saved_ptr = *live_ptr;
        saved_count = *live_count;

        *buf = exclude_tag;
        if (!DMK::memory::write_in_place(list_field, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buf))))
            return false;
        if (!DMK::memory::write_in_place(count_field, static_cast<std::uint32_t>(1)))
        {
            // Put the list pointer back. A component that keeps the pointer without the count strands a dead stack
            // address in the resolver's exclusion list.
            (void)DMK::memory::write_in_place(list_field, saved_ptr);
            return false;
        }
        return true;
    }

    /**
     * @brief Undoes @ref arm_slot_exclusion.
     * @param a1 The live equip-slot component.
     * @param saved_ptr List pointer @ref arm_slot_exclusion captured.
     * @param saved_count Count @ref arm_slot_exclusion captured.
     * @details It must run on every path out, including an unwind through the equip. A failed restore leaves the
     *          engine's exclusion list naming a dead stack buffer, so the failure is logged rather than swallowed.
     */
    static void disarm_slot_exclusion(std::int64_t a1, std::uint64_t saved_ptr, std::uint32_t saved_count) noexcept
    {
        if (!plausible_engine_ptr(a1))
            return;

        const DMK::Address list_field{static_cast<std::uintptr_t>(a1 + COMP_SLOT_EXCLUDE_LIST_OFFSET)};
        const DMK::Address count_field{static_cast<std::uintptr_t>(a1 + COMP_SLOT_EXCLUDE_COUNT_OFFSET)};
        const bool list_restored = DMK::memory::write_in_place(list_field, saved_ptr).has_value();
        const bool count_restored = DMK::memory::write_in_place(count_field, saved_count).has_value();
        if (!list_restored || !count_restored)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "[dispatch] slot-exclusion restore FAILED a1={:#x} list={} count={} - the resolver keeps a dead list",
                static_cast<std::uint64_t>(a1),
                list_restored,
                count_restored
            );
        }
    }

    /**
     * @brief Reports whether `slot_tag` names a live part record.
     * @param resolve Engine slot-tag to handle resolver.
     * @param a1 The live equip-slot component.
     * @param slot_tag Engine slot tag to test.
     * @return true when the tag resolves to a handle.
     * @details POD-only frame, so the SEH guard around the engine call is legal.
     */
    static bool slot_tag_is_live_seh(SlotTagToHandleFn resolve, __int64 a1, std::uint16_t slot_tag) noexcept
    {
        if (!resolve)
            return false;
        __try
        {
            std::uint16_t handle = NO_GAME_TAG;
            resolve(a1, &handle, slot_tag, 0);
            return handle != NO_GAME_TAG;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    /**
     * @brief Resolve the slot tag to its handle, then refresh - under SEH, in a POD-only frame.
     *
     * @details PartSlotRefresh's two slot arguments are NOT the same namespace. The first is a TAG, matched against
     *          bucket keys and against the part record's own tag field. The second is a HANDLE, which the function
     *          dereferences through a lookup. Passing the tag for both faults.
     * @return false when either pointer is null, the tag names no live part record, or the call faulted.
     */
    static bool call_part_slot_refresh_seh(
        PartSlotRefreshFn fn,
        SlotTagToHandleFn resolve,
        __int64 a1,
        std::uint16_t slot_tag,
        __int64 swap_entry
    ) noexcept
    {
        if (!fn || !resolve)
            return false;
        __try
        {
            std::uint16_t handle = NO_GAME_TAG;
            resolve(a1, &handle, slot_tag, 0);
            if (handle == NO_GAME_TAG)
                return false; // tag names no live part record - nothing to refresh
            fn(a1, static_cast<__int16>(slot_tag), static_cast<__int16>(handle), swap_entry);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The SlotPopulator choke point for every apply.
    //
    // `id` is the descriptor id fed to the engine - always an item the wearer can legitimately equip, because the
    // transmog VISUAL no longer comes from the descriptor. It comes from the prefab-wrapper swap, which redirects the
    // mesh this item otherwise renders. Nothing here has to defeat an equip gate.
    //
    // `slot_sel` chooses WHICH engine slot receives the item. SlotPopulator reads the u16 at itemData+12, and
    // `NO_GAME_TAG` means "derive the slot from the item", which the engine resolves through an item -> slot lookup.
    // For a PAIRED slot that derivation can only ever produce one answer - both halves share one item type - so the
    // second half is unreachable and its carrier lands in the first. Naming the slot explicitly reaches the other half.
    static void
    apply_transmog_core(__int64 a1, uint16_t id, uint16_t slot_sel = NO_GAME_TAG, uint16_t exclude_tag = NO_GAME_TAG)
    {
        auto slot_pop = slot_populator_fn();
        auto init_entry = init_swap_entry_fn();
        if (!slot_pop || !init_entry)
            return;

        // Build 16-byte item data structure for SlotPopulator. Layout matches a natural-engine equip exactly:
        //   XX YY 02 00 00 00 00 00 FF FF FF FF FF FF 00 00
        // The engine validates the +4..+11 region as part of its dye/material-instance lookup. If those dwords are
        // reordered, the engine falls back to default colors even when the wrapper-swap mesh is correct.
        // Settle the destination BEFORE the struct is built - itemData+12 is written below, and a later change to
        // this variable never reaches the engine.
        //
        // An explicit destination needs the slot to already own a part record: SlotPopulator resolves the tag first
        // and bails outright when it cannot, equipping nothing. Fall back to derivation rather than refuse.
        bool exclude_first_half = false;
        if (slot_sel != NO_GAME_TAG && !slot_tag_is_live_seh(slot_tag_to_handle_fn(), a1, slot_sel))
        {
            // Derivation alone lands on the FIRST half of the pair. An exclusion of that half from resolution
            // lands it here instead, and it names no destination the engine refuses.
            exclude_first_half = (exclude_tag != NO_GAME_TAG);
            if (exclude_first_half)
                DMK::log().debug(
                    "[dispatch] slot {:#06x} has no live part record - deriving with {:#06x} excluded",
                    slot_sel,
                    exclude_tag
                );
            else
                DMK::log().debug("[dispatch] slot {:#06x} has no live part record - deriving", slot_sel);
            slot_sel = NO_GAME_TAG;
        }

        alignas(16) uint8_t itemData[16]{};
        *reinterpret_cast<uint16_t *>(itemData + 0) = id;
        itemData[2] = 2;
        // bytes 4..7 left as 0 (zero-init)
        *reinterpret_cast<uint32_t *>(itemData + 8) = 0xFFFFFFFF;
        *reinterpret_cast<uint16_t *>(itemData + 12) = slot_sel;

        // Build empty swap entry.
        alignas(16) uint8_t swap_entry[256]{};
        init_entry(reinterpret_cast<__int64>(swap_entry));

        in_transmog().store(true, std::memory_order_relaxed);
        // Reset the host-scope cluster so the upcoming slot_pop's matInst-iter hits build a fresh player-vs-NPC
        // histogram.
        color_override::host_scope::begin_apply_window();
        // Open the setter-property substitute window. Any 4-byte material-property write the engine fires during
        // slot_pop goes to the user's chosen RGB. The window closes again immediately after, so unrelated render passes
        // are not tinted.
        color_override::setter_substitute::set_apply_window(true);
        // slot_pop faults (structured exception) on early load before game data is ready. __finally restores the apply
        // window and the in_transmog() flag even on an SEH unwind. A stranded apply window tints unrelated render
        // passes. A stranded in_transmog() leaves the wrapper swap armed outside its window.
        //
        // The return is CAPTURED, not discarded: its low word is the engine's answer, and `NO_GAME_TAG` there means
        // the call refused and equipped nothing. It is not trusted as the ONLY signal, though. The return also
        // carries pointer bits on some paths, so a low word that is not `NO_GAME_TAG` does not by itself prove the
        // carrier was placed. `derived_tag` below is the independent check that closes that gap.
        //
        // `slot_pop_completed` is what separates a REFUSAL from a FAULT. slot_pop_rc starts at -1, and the low word of
        // -1 is the same `NO_GAME_TAG` a refusal returns, so without the flag a fault inside the call - which skips the
        // assignment entirely - reports as "REFUSED, nothing was equipped": identical symptom, different cause.
        alignas(2) std::uint16_t exclusion_buf = NO_GAME_TAG;
        std::uint64_t saved_excl_ptr = 0;
        std::uint32_t saved_excl_count = 0;
        const bool exclusion_armed =
            exclude_first_half && arm_slot_exclusion(a1, &exclusion_buf, exclude_tag, saved_excl_ptr, saved_excl_count);

        // Pre-check the destination the way SlotPopulator will derive it, through the same engine resolver it calls
        // first. With `slot_sel == NO_GAME_TAG` the engine derives the slot from the item, and a derivation that
        // answers `NO_GAME_TAG` makes the whole call a NO-OP: it equips nothing and leaves the slot EMPTY, which
        // reads as a broken transmog rather than a carrier the engine will not place. Asking up front is what turns
        // that into a warning naming the item.
        //
        // The function pointer is kept so the warning below can tell a genuine refusal from an unresolved cascade.
        // item_to_slot_seh answers NO_GAME_TAG for both, and a warning on the second blames the user's carrier for
        // a resolve failure, on every enabled slot, on every apply.
        const auto slot_resolve_fn = item_to_slot_resolve_fn();
        const auto derived_tag = item_to_slot_seh(slot_resolve_fn, a1, id);

        std::int64_t slot_pop_rc = -1;
        bool slot_pop_completed = false;
        bool refresh_called = false;
        bool refresh_faulted = false;
        const auto refresh_fn = part_slot_refresh_fn();
        __try
        {
            slot_pop_rc =
                slot_pop(a1, reinterpret_cast<unsigned __int16 *>(itemData), reinterpret_cast<__int64>(swap_entry));
            slot_pop_completed = true;

            // Refresh the targeted slot INSIDE the apply window, exactly where the engine runs its own refresh.
            //
            // SlotPopulator files the entry under `slot_sel` but rebuilds the slot it DERIVED FROM THE ITEM. Both
            // halves of a paired slot derive the same value, so the second half's entry lands correctly while the
            // first half is the one the engine rebuilds. A second rebuild with the targeted slot in BOTH argument
            // positions covers the half the engine skipped.
            //
            // The placement matters as much as the call. Run after the __finally below, this rebuilds with the dye
            // substitute window shut and in_transmog() already cleared: the prefab swap does not substitute and the
            // dye writes are not intercepted, so the part flickers to its untransmogged mesh and armor loses its
            // color a moment after appearing. Both windows have to still be open.
            //
            // Only for an explicit slot_sel - with `NO_GAME_TAG` the engine's own derivation is already right.
            if (slot_sel != NO_GAME_TAG && static_cast<std::uint16_t>(slot_pop_rc & 0xFFFF) != NO_GAME_TAG &&
                refresh_fn)
            {
                refresh_called = call_part_slot_refresh_seh(
                    refresh_fn,
                    slot_tag_to_handle_fn(),
                    a1,
                    slot_sel,
                    reinterpret_cast<__int64>(swap_entry)
                );
                refresh_faulted = !refresh_called;
            }
        }
        __finally
        {
            color_override::setter_substitute::set_apply_window(false);
            in_transmog().store(false, std::memory_order_relaxed);
        }

        // Always restore, including on an SEH unwind through the block above.
        if (exclusion_armed)
            disarm_slot_exclusion(a1, saved_excl_ptr, saved_excl_count);

        // Logging lives outside the __try: string formatting needs object unwinding, which cannot coexist with SEH
        // in the same frame.
        const auto rc_word = static_cast<std::uint16_t>(slot_pop_rc & 0xFFFF);
        if (!slot_pop_completed)
        {
            DMK::log().warning(
                "[dispatch] SlotPopulator FAULTED item={:#06x} slotSel={:#06x} - the call raised, it did not refuse",
                id,
                slot_sel
            );
        }
        else if (rc_word == NO_GAME_TAG)
        {
            DMK::log().warning(
                "[dispatch] SlotPopulator REFUSED item={:#06x} slotSel={:#06x} - nothing was equipped",
                id,
                slot_sel
            );
        }
        else
        {
            // `derived_tag` is the slot the engine itself resolved the item to. With slot_sel == NO_GAME_TAG that is
            // the engine's choice, so the log line separates "the carrier landed in the requested slot" from "it
            // landed elsewhere and the requested slot stayed empty". Both outcomes otherwise read as "ok".
            DMK::log().trace(
                "[dispatch] SlotPopulator ok item={:#06x} slotSel={:#06x} derivedTag={:#06x} refresh={}",
                id,
                slot_sel,
                derived_tag,
                refresh_faulted ? "FAULTED" : (refresh_called ? "yes" : "no")
            );
            if (slot_resolve_fn && slot_sel == NO_GAME_TAG && derived_tag == NO_GAME_TAG)
            {
                DMK::log().warning(
                    "[dispatch] carrier {:#06x} has NO slot mapping - SlotPopulator placed nothing and the slot "
                    "stays empty. Pick a carrier the engine can place, or name the slot explicitly.",
                    id
                );
            }
        }
    }

    /**
     * @brief Drive the engine's per-slot rebuild with the prefab-swap and color windows open.
     *
     * @details Internal on purpose. It publishes NO dye of its own, so a call after the apply path clears the dye
     *          state makes the rebuild's DyeCopier call re-emit the engine's natural records and strip the injected
     *          color. Every caller goes through @ref refresh_slot_appearance, which brackets it with the
     *          dye publish and the color_override slot bind.
     * @return false when an anchor is unresolved, the slot has no live part record, or the call faulted.
     */
    static bool refresh_slot_visual(TransmogSlot slot)
    {
        auto &logger = DMK::log();

        const auto a1 = static_cast<__int64>(player_a1().load(std::memory_order_acquire));
        if (!a1)
            return false;
        const auto tag = game_slot_from_transmog(slot);
        if (tag < 0)
            return false;

        const auto refresh = part_slot_refresh_fn();
        const auto resolve = slot_tag_to_handle_fn();
        const auto init_entry = init_swap_entry_fn();
        if (!refresh || !resolve || !init_entry)
            return false;

        // An EMPTY swap entry on purpose. PartSlotRefresh falls back to the entry already registered for the slot
        // when its 4th argument is the sentinel or empty, so a rebuild needs no item id and no equip - it reuses
        // whatever is installed and re-runs the build.
        alignas(16) std::uint8_t swap_entry[256]{};
        init_entry(reinterpret_cast<__int64>(swap_entry));

        // Both windows open, exactly as an apply does. in_transmog keeps the prefab swap substituting, so the
        // transmogged mesh survives the rebuild in place of a revert to the carrier. The setter window routes the
        // engine's material writes to the chosen color.
        //
        // The order carries the invariant. A rebuild with these windows shut brings armor up dyed and reverts it a
        // moment later.
        in_transmog().store(true, std::memory_order_relaxed);
        color_override::host_scope::begin_apply_window();
        color_override::setter_substitute::set_apply_window(true);

        const bool ok = call_part_slot_refresh_seh(
            refresh,
            resolve,
            a1,
            static_cast<std::uint16_t>(tag),
            reinterpret_cast<__int64>(swap_entry)
        );

        color_override::setter_substitute::set_apply_window(false);
        in_transmog().store(false, std::memory_order_relaxed);

        logger.debug(
            "[dispatch] refresh_slot_visual slot={} tag={:#06x} -> {}",
            slot_name(slot),
            static_cast<std::uint16_t>(tag),
            ok ? "ok" : "failed"
        );
        return ok;
    }

    bool refresh_slot_appearance(std::size_t slot_idx)
    {
        if (slot_idx >= SLOT_COUNT)
            return false;
        const auto slot = static_cast<TransmogSlot>(slot_idx);

        // Publish the dye state a full apply publishes, then rebuild in place of a re-equip. The injector's detour
        // consumes this on the next DyeCopier call, which the rebuild drives, so the records land with no tear-down
        // and no second equip.
        publish_preset_dye_for_slot(slot_idx);

        color_override::setter_substitute::set_active_slot(static_cast<int>(slot_idx));
        const bool ok = refresh_slot_visual(slot);
        dye_record_inject::clear_slot_dye_state();

        DMK::log()
            .debug("[dispatch] refresh_slot_appearance slot={} -> {}", slot_name(slot), ok ? "rebuilt" : "unavailable");
        return ok;
    }

    void apply_transmog(__int64 a1, uint16_t targetId)
    {
        apply_transmog_core(a1, targetId);
    }

    // - Default carrier set (per character)
    // Each entry must be a valid item for THAT character in the given slot - something the engine's equip class-gate
    // accepts. Kliff and Oongka share Kliff_PlateArmor_* because the engine treats their equip class the same. Damiane
    // has her own armor namespace (Demian_*) which Kliff cannot wear, but the engine accepts those items on Damiane
    // even though the item catalog marks them PlayerSafe=no. The PlayerSafe flag reflects Kliff compatibility only.
    //
    // If a name fails to resolve, the slot falls back to direct equip, which can fail silently for NPC/variant items.

    // Per-character default carrier item-names live in carrier_defaults.hpp::CARRIERS[character][slot].item_name.
    // ItemNameTable resolves each to a uint16_t carrier item_id at runtime. This function picks the right row for the
    // active character and falls back to Kliff if the character-specific entry is not catalog-resident.
    uint16_t default_carrier_for_slot(TransmogSlot slot, std::string_view char_name)
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= SLOT_COUNT)
            return 0;
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
            return 0;

        const auto char_opt = carrier_char_from_name(char_name);
        const auto cc = char_opt.value_or(CarrierChar::Kliff);

        const char *name = carrier_for(cc, slot).item_name;
        auto id = table.id_of(name);
        if (id.has_value())
            return *id;

        // Fallback: if a character-specific carrier name did not resolve (missing from catalog, renamed), try Kliff's
        // set.
        if (cc != CarrierChar::Kliff)
        {
            auto kliff = table.id_of(carrier_for(CarrierChar::Kliff, slot).item_name);
            return kliff.value_or(0);
        }
        return 0;
    }

    /**
     * @brief Second-pass tear-down for a DIRECT-applied fake - one whose carrier collapsed onto the target, or that
     *        had no carrier at all.
     *
     * @details Such a fake renders its per-body rig through the engine's own variant resolver, and the engine needs
     *          the scene-graph tear-down fired TWICE to detach that rig fully. The normal Phase A / Phase B flow only
     *          supplies the second call when a DISTINCT carrier is torn, or when the live real item equals the fake
     *          and Phase B re-tears the same hash. A direct fake with no matching real underneath - a mask transmog
     *          on a wearer who owns no real mask - gets a single call, and its rendered rig survives. The symptom
     *          is a fake accessory that stays visible after a switch to a none-preset.
     * @note No-op when a distinct carrier already handled the tear, when Phase B handles it, or when there is no
     *       fake. SafeTearDown does not mutate the authoritative entry array, so a redundant detach of an
     *       already-gone rig is a safe no-op.
     */
    static void tear_down_direct_fake_second_pass(
        __int64 a1,
        std::uint16_t fake_id,
        std::uint16_t game_tag,
        std::uint16_t live_real_id,
        bool distinct_carrier_torn
    ) noexcept
    {
        if (fake_id == 0 || distinct_carrier_torn || live_real_id == fake_id)
            return;
        real_part_tear_down::tear_down_by_item_id(reinterpret_cast<void *>(a1), fake_id, game_tag);
        real_part_tear_down::tear_down_by_item_id(reinterpret_cast<void *>(a1), fake_id, game_tag);
    }

    void apply_transmog_with_carrier(
        __int64 a1,
        uint16_t carrier_id,
        uint16_t targetId,
        uint16_t slot_sel,
        uint16_t exclude_tag
    )
    {
        auto &logger = DMK::log();

        // The carrier is equipped AS ITSELF. It does not impersonate the target.
        //
        // The prefab-wrapper swap is what delivers the visual: the swap map binds the carrier's own prefab to the
        // target item's prefab, so the engine renders the target mesh while never being told anything untrue about
        // the item. Nothing here copies a descriptor, patches the catalog pointer array, or defeats an equip gate -
        // an item the wearer cannot normally equip needs none of that, because the carrier is always something the
        // wearer CAN equip.
        if (carrier_id == 0)
        {
            logger.trace("[carrier] no carrier resolved for target={:#06x}, applying it directly", targetId);
            // Equipped as itself with no swap behind it - register so the post-apply sweep can find it later.
            prefab_wrapper_swap::register_direct_fake(targetId);
            apply_transmog_core(a1, targetId, slot_sel, exclude_tag);
            return;
        }

        // Carrier IS the target: the item is equipped as itself and no substitution delivers the visual, so nothing
        // reaches on_struct_copy and the sweep never learns this slot was filled.
        if (carrier_id == targetId)
            prefab_wrapper_swap::register_direct_fake(targetId);

        logger.trace(
            "[carrier] equipping carrier={:#06x} as itself (visual for target={:#06x} comes from the prefab swap)",
            carrier_id,
            targetId
        );
        apply_transmog_core(a1, carrier_id, slot_sel, exclude_tag);
    }

    /**
     * @brief Confirms the body an apply targets belongs to the character the apply is for.
     * @param a1 The live equip-slot component the apply runs against.
     * @param site Static label for the log line.
     * @return true when the body matches a preset axis, or when either side is unresolved.
     * @details LT picks WHAT to apply from `current_apply_owner()`, a name held in PresetManager, and WHERE to apply
     *          it from the `a1` it was handed. A window where one updates before the other lands a character's preset
     *          on a different character's body.
     *
     *          It blocks only when BOTH sides resolve AND they disagree. A zero host index means the body is not a
     *          protagonist at all (a targeted apply onto a companion, which is legitimate) or the actor chain is
     *          mid-teardown. Neither is a mismatch, and a block there breaks working paths.
     */
    static bool apply_host_matches_owner(std::uintptr_t a1, const char *site) noexcept
    {
        // Live walk, not the published table. This guard fails open on 0, so answering "unknown" disarms it
        // silently, and the table reads unknown for a stretch after every world load. The apply pipeline runs a few
        // times a second and already paid this walk, so it can afford the certainty. The engine-thread hooks cannot.
        const auto host_idx = char_idx_for_equip_slot_uncached(a1);
        if (host_idx == 0)
            return true; // not a protagonist body, or the chain is mid-teardown - not a mismatch

        // Accept the host if it matches EITHER preset axis.
        //
        // current_apply_owner() alone is not a safe comparand: it returns the pinned EDITING character when pinning
        // is engaged, and the world-entry loop legitimately walks every protagonist in turn with
        // set_active_character() moving underneath it. A compare against the owner alone blocks every iteration
        // but one. A body that matches neither axis is the actual defect this guard exists for.
        auto &pm = PresetManager::instance();
        const auto active_idx = CDCore::character_idx_from_name(pm.active_character());
        const auto editing_idx =
            pm.editing_pinned() ? CDCore::character_idx_from_name(pm.editing_character()) : std::uint32_t{0};
        if (active_idx == 0 && editing_idx == 0)
            return true; // nothing bound yet - the caller's own gating decides
        if (host_idx == active_idx || host_idx == editing_idx)
            return true;

        DMK::log().warning(
            "{}: HOST MISMATCH - a1 0x{:X} is charIdx {}'s body, but the preset axes are active='{}' ({}) "
            "editing='{}' ({}); skipping so the wrong character is not dressed",
            site,
            static_cast<std::uint64_t>(a1),
            host_idx,
            pm.active_character(),
            active_idx,
            pm.editing_pinned() ? pm.editing_character() : std::string{},
            editing_idx
        );
        return false;
    }

    void apply_single_slot_transmog(__int64 a1, std::size_t slot_idx)
    {
        if (slot_idx >= SLOT_COUNT)
            return;

        auto &logger = DMK::log();
        auto &mappings = slot_mappings();
        auto &last_ids = last_applied_ids();
        auto &m = mappings[slot_idx];

        // resolve_player_component() walks WorldSystem -> ActorManager -> UserActor -> actor and always returns
        // Kliff's component, whatever character the user controls. An unconditional call clobbers the per-character a1
        // values that the VEC / BatchEquip hooks pass in. Keep it ONLY as a fallback when the passed-in a1 is invalid.
        if (!plausible_engine_ptr(a1) && world_system_ptr().load(std::memory_order_acquire))
        {
            const auto fresh = resolve_player_component();
            if (plausible_engine_ptr(fresh))
                a1 = fresh;
        }

        if (!apply_host_matches_owner(static_cast<std::uintptr_t>(a1), "apply_single_slot"))
            return;

        const auto single_slot_actor = read_component_actor(a1);
        if (!single_slot_actor)
        {
            logger.warning("apply_single_slot: a1 access fault");
            return;
        }
        if (!plausible_engine_ptr(static_cast<__int64>(*single_slot_actor)))
        {
            logger.warning("apply_single_slot: a1 invalid");
            return;
        }

        const uint16_t prev_id = last_ids[slot_idx];
        const uint16_t game_tag = static_cast<uint16_t>(SLOT_METADATA[slot_idx].game_tag);

        // Compute target: active slot with non-zero id -> transmog, otherwise clear this slot.
        const uint16_t targetId = (m.active && m.target_item_id != 0) ? m.target_item_id : 0;

        // One-shot force flag set by the body-mesh picker when it re-picks a prefab on the same carrier id. The flag
        // bypasses the equality early-out, so Phase A still runs against the real prev_id. Phase A then drives the
        // engine's natural-pipeline hook to clean up the prior tgt wrapper. Read-and-clear.
        const bool force_apply = force_apply_pending()[slot_idx];
        if (force_apply)
            force_apply_pending()[slot_idx] = false;

        // Early-out: nothing changed for this slot.
        if (targetId == prev_id && targetId != 0 && !force_apply)
        {
            logger.trace("apply_single_slot: slot={} id={:#06x} unchanged", slot_idx, targetId);
            return;
        }

        // Scoped dispatch cache clear
        // Walk the 24-byte stride cache and zero subCount only for entries whose slotNativeId matches this slot's game
        // tag. This leaves other slots' blobs untouched, so VEC does not re-dispatch them.
        __try
        {
            const auto count = *reinterpret_cast<volatile uint32_t *>(a1 + COMP_SLOT_CACHE_COUNT_OFFSET);
            const auto base = *reinterpret_cast<volatile uintptr_t *>(a1 + COMP_SLOT_CACHE_BASE_PTR_OFFSET);
            if (plausible_engine_ptr(static_cast<__int64>(base)))
            {
                for (uint32_t e = 0; e < count; ++e)
                {
                    const auto entry = base + 24ULL * e;
                    const auto slot_id = *reinterpret_cast<volatile uint16_t *>(entry);
                    if (slot_id == game_tag)
                        *reinterpret_cast<volatile uint32_t *>(entry + 0x10) = 0;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("[dispatch] single-slot cache clear fault");
            return;
        }

        // Tear-down scoped to this slot
        std::uint16_t real_id = 0;
        if (real_part_tear_down::is_ready())
        {
            real_id = real_part_tear_down::get_real_item_id(reinterpret_cast<void *>(a1), game_tag);

            // Phase A: tear down previous fake. Runs even when the previous fake item_id matches the live real item.
            // Fake and real get the same treatment, so the tear-down/apply sequence is always complete.
            if (prev_id != 0)
            {
                const auto prev_carrier = last_applied_carrier_ids()[slot_idx];
                if (prev_carrier != 0 && prev_carrier != static_cast<uint16_t>(prev_id))
                {
                    real_part_tear_down::tear_down_by_item_id(reinterpret_cast<void *>(a1), prev_carrier, game_tag);
                }
                real_part_tear_down::tear_down_by_item_id(
                    reinterpret_cast<void *>(a1),
                    static_cast<uint16_t>(prev_id),
                    game_tag
                );

                // Direct-applied fake with no matching real underneath: it needs a second detach.
                // See tear_down_direct_fake_second_pass.
                tear_down_direct_fake_second_pass(
                    a1,
                    static_cast<std::uint16_t>(prev_id),
                    game_tag,
                    real_id,
                    prev_carrier != 0 && prev_carrier != static_cast<std::uint16_t>(prev_id)
                );
            }

            // Phase B: tear down the real item. Runs unconditionally (fake == real is treated the same as fake !=
            // real).
            if (targetId != 0)
            {
                if (real_part_tear_down::tear_down_real_part(reinterpret_cast<void *>(a1), game_tag))
                    real_damaged()[slot_idx] = true;
            }
        }

        // Apply
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
            // apply re-installs. The full path gets this from the deactivate cycle inside notify_apply_starting.
            // The scope of one slot is what keeps the other slots' live targets off the victim list.
            if (prev_id != 0 && prev_id != targetId)
                prefab_wrapper_swap::park_slot_target_for_sweep(prev_id);

            prefab_wrapper_swap::ensure_armed_for_slot_apply();

            const auto tm_slot = static_cast<TransmogSlot>(slot_idx);
            // Use current_apply_owner so a targeted-apply onto a non-controlled body resolves its carrier from THAT
            // body's defaults. PresetManager::active_character() returns the controlled character, which under
            // pin+flag is the wrong axis. A carrier mismatch installs the wrong wrapper family on the body and
            // produces visual cross-talk through the swap map.
            const auto &active_char = current_apply_owner();
            // Every slot goes through a carrier now. The carrier supplies a legitimately equippable item and the
            // prefab swap supplies the visual. No "can the wearer equip the target" question remains to branch on.
            const uint16_t carrier_id = default_carrier_for_slot(tm_slot, active_char);
            const bool use_carrier = carrier_id != 0;

            // Same dye plumbing as apply_all_transmog. Without this, a single-slot apply (manual_apply_slot from
            // the dye picker) bypasses the injector and the engine's natural records dominate.
            publish_preset_dye_for_slot(slot_idx);

            color_override::setter_substitute::set_active_slot(static_cast<int>(slot_idx));
            // Notify color_override of the user-INTENDED target item for this slot. This wipes the swatch table only
            // when the user's chosen transmog target ACTUALLY changes. It does not wipe when the resolved target flips
            // to the carrier for the duration of an untick, which the dispatch path does.
            //
            // Pass the picked target when slot is active, 0 when unticked. Matches notify_transmog_target's contract:
            // 0 = "no transmog this slot", non-zero = the fake target the user wants to wear. Wipe fires only on
            // (non-zero last) -> (different non-zero new).
            {
                auto &mapping = slot_mappings()[slot_idx];
                const std::uint32_t user_intent =
                    mapping.active ? static_cast<std::uint32_t>(mapping.target_item_id) : 0u;
                color_override::reinit::notify_transmog_target(static_cast<int>(slot_idx), user_intent);
            }
            if (use_carrier && carrier_id != 0)
            {
                logger.debug(
                    "apply_single_slot: slot={} target={:#06x} carrier={:#06x}",
                    slot_name(tm_slot),
                    targetId,
                    carrier_id
                );
                apply_transmog_with_carrier(
                    a1,
                    carrier_id,
                    targetId,
                    slot_needs_explicit_destination(tm_slot) ? static_cast<uint16_t>(game_slot_from_transmog(tm_slot))
                                                             : NO_GAME_TAG,
                    paired_first_half_tag(tm_slot)
                );
            }
            else
            {
                logger.debug("apply_single_slot: slot={} target={:#06x}", slot_name(tm_slot), targetId);
                apply_transmog(a1, targetId);
            }

            dye_record_inject::clear_slot_dye_state();

            // Detach the replaced target now that the new one is installed. Mirrors notify_apply_finished, which
            // the single-slot path does not call.
            prefab_wrapper_swap::sweep_after_slot_apply();

            // Rebuild through refresh_slot_appearance, NOT the bare refresh_slot_visual.
            //
            // The rebuild drives a DyeCopier call and the dye state was cleared above, so a bare rebuild re-emits
            // the slot with the engine's natural records and the transmog loses its color.
            // refresh_slot_appearance republishes this slot's dye (and its color_override slot) around the rebuild,
            // which is what the injector's detour consumes.
            //
            // Mirrors the loop apply_all_transmog runs after notify_apply_finished.
            //
            // Every slot installs through a carrier, and the carrier for a given slot does not change when the user
            // picks a different target - only the swap map does. So the equip layer sees the same item go back on,
            // has nothing to reconcile, and leaves the previously realized mesh exactly where it is. Erasing the old
            // claim does not retract it either. The engine only reconciles on a rebuild.
            //
            // The rebuild keeps both apply paths equal on a target change. Without it the single-slot path leaves
            // the previous item on screen after an Instant Apply pick.
            if (prev_id != targetId)
                refresh_slot_appearance(slot_idx);

            last_ids[slot_idx] = targetId;
            last_applied_carrier_ids()[slot_idx] = (use_carrier && carrier_id != 0) ? carrier_id : 0;
            // Phase B set real_damaged when it tore down the real item for this slot. The fake is applied now, so
            // clear the flag and keep apply_all_transmog from seeing stale damage state on later cycles.
            real_damaged()[slot_idx] = false;
        }
        else
        {
            // Clearing this slot. Two cases:
            //  - active + none (checkbox ticked, picker = none): user wants to show an EMPTY slot (bare head, etc.).
            //    Do NOT restore the real item. It is already gone: Phase B of the call that INSTALLED the fake tore
            //    it down, and nothing has put it back since. This call's Phase B is gated on `targetId != 0` and does
            //    not run, so nothing here removes it - leaving it alone is what keeps the slot empty.
            //  - inactive (!m.active): LT controlled the slot before, so restore the real item and it reappears.
            const bool show_empty = m.active;
            // During a 3-pass reinit cycle, suppress the real-armor restore so the slot goes visibly empty between
            // teardown and retick instead of flashing the real armor on every cycle.
            const bool reinit_active = color_override::reinit::is_slot_reinit_active(static_cast<int>(slot_idx));
            if (!show_empty && (prev_id != 0 || real_damaged()[slot_idx]) && !reinit_active)
            {
                if (real_id != 0)
                {
                    logger.debug(
                        "apply_single_slot: slot={} restoring real {:#06x}",
                        slot_name(static_cast<TransmogSlot>(slot_idx)),
                        real_id
                    );
                    color_override::setter_substitute::set_active_slot(static_cast<int>(slot_idx));
                    apply_transmog(a1, real_id);
                }
            }
            else if (reinit_active)
            {
                logger.debug(
                    "apply_single_slot: slot={} real-restore SKIPPED (reinit teardown - slot goes empty by design)",
                    slot_idx
                );
            }
            last_ids[slot_idx] = 0;
            last_applied_carrier_ids()[slot_idx] = 0;
            // Clear damage flag so the slot is fully released back to the game. Without this, apply_all_transmog's
            // untick-restore and slot_needs_work checks see stale damage state and keep interfering with an unmanaged
            // slot.
            real_damaged()[slot_idx] = false;
        }

        // Update suppress mask for this slot only. Rebuild full mask from current state rather than toggling one bit,
        // to stay consistent with apply_all_transmog's mask logic.
        std::uint32_t suppress_mask = 0;
        for (std::size_t k = 0; k < SLOT_COUNT; ++k)
        {
            const auto &sm = mappings[k];
            if (!sm.active)
                continue;
            const std::uint16_t slot_real = real_part_tear_down::is_ready()
                                                ? real_part_tear_down::get_real_item_id(
                                                      reinterpret_cast<void *>(a1),
                                                      static_cast<std::uint16_t>(SLOT_METADATA[k].game_tag)
                                                  )
                                                : 0;
            if (sm.target_item_id != 0 && static_cast<uint16_t>(sm.target_item_id) == slot_real)
                continue;
            suppress_mask |= (std::uint32_t{1} << k);
        }
        part_show_suppress::set_mask(suppress_mask);

        logger.trace("apply_single_slot: slot={} done, suppress={:#x}", slot_idx, suppress_mask);
    }

    /**
     * @brief Reports the slot the engine resolves for each enabled slot's carrier.
     * @param a1 The live equip-slot component.
     * @param char_name Character whose default carriers the report covers.
     * @details SlotPopulator resolves the item to a slot before it does anything and refuses outright on 0xFFFF, so
     *          a carrier that does not resolve equips nothing and the slot silently stays as it was. The log line is
     *          the only thing that surfaces that failure.
     *
     *          Both halves of a paired slot resolve to the FIRST slot's tag, because they share one equip type. That
     *          is why the second half needs its destination named outright.
     */
    static void log_carrier_resolution(__int64 a1, const std::string &char_name)
    {
        const auto fn = item_to_slot_resolve_fn();
        if (!fn)
            return;
        std::string line;
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            const auto sl = static_cast<TransmogSlot>(i);
            if (!slot_enabled(sl))
                continue;
            const auto carrier = default_carrier_for_slot(sl, char_name);
            if (carrier == 0)
                continue;
            const auto placed = item_to_slot_seh(fn, a1, carrier);
            if (!line.empty())
                line += ", ";
            line += std::format(
                "{}:{:#06x}->{}",
                slot_name(sl),
                carrier,
                placed == 0xFFFF ? std::string{"REFUSED"} : std::format("{:#06x}", placed)
            );
        }
        DMK::log().trace("[dispatch] carrier resolution [{}]", line);
    }

    void apply_all_transmog(__int64 a1)
    {
        auto &logger = DMK::log();
        // Local copy of slot_mappings. It carries a synthesized "all-slots-cleared" view when the user toggles LT off,
        // and it leaves the persisted preset state untouched. See the flag_enabled() block immediately below.
        auto mappings = slot_mappings();
        auto &last_ids = last_applied_ids();

        // An Enabled=off toggle must NOT early-out of the dispatcher. An early return freezes the cleanup pass (the
        // TEAR_DOWN_SLOTS loop with mappings[idx].active==false + real_item_id==0 + last_applied_real_ids[idx]!=0), and
        // stale restore meshes from a prior LT apply then leak through the next organic radial unequip. Instead, force
        // every mapping inactive in this local copy and let the dispatcher run as if the user unticked every slot.
        // The cleanup pass keeps tearing down stale fakes. The apply pass writes nothing, because every slot has
        // active==false.
        //
        // No equip event schedules this pass. While LT is disabled it runs only from the UI, a hotkey, or world
        // entry. socket_mesh_override installs nothing while disabled either, so no new fake appears in the meantime.
        if (!flag_enabled().load(std::memory_order_relaxed))
        {
            for (auto &m : mappings)
            {
                m.active = false;
                m.target_item_id = 0;
            }
        }

        // Fallback only - see apply_single_slot_transmog comment.
        if (!plausible_engine_ptr(a1) && world_system_ptr().load(std::memory_order_acquire))
        {
            const auto fresh = resolve_player_component();
            if (plausible_engine_ptr(fresh))
                a1 = fresh;
        }

        if (!apply_host_matches_owner(static_cast<std::uintptr_t>(a1), "apply_all_transmog"))
            return;

        const auto all_slots_actor = read_component_actor(a1);
        if (!all_slots_actor)
        {
            logger.warning("apply_all_transmog: a1 access fault");
            return;
        }
        if (!plausible_engine_ptr(static_cast<__int64>(*all_slots_actor)))
        {
            logger.warning("apply_all_transmog: a1 invalid");
            return;
        }

        // Engine-readiness gate. is_world_ready() observes the world singleton, which becomes non-null well before the
        // per-actor sub-handler at CCC+0x130 is wired. If the dispatcher runs against an actor whose sub-handler is
        // still null, the engine path under tear_down / tear_down_fake dereferences it as `v15 = *(v7 + 304); *v15;`.
        // The per-slot SEH wrappers then burst-catch the faults, and every slot exits early. The carrier apply
        // continues against a half-wired actor before it raises out of the outer __try. The gate at this entry point
        // covers every caller (manual_apply, the multi-protagonist worker path, any future trigger) and does not
        // couple them to LT-specific readiness semantics.
        constexpr std::uint64_t apply_ready_retry_ms = 1000;
        if (!real_part_tear_down::is_actor_apply_ready(reinterpret_cast<void *>(a1)))
        {
            logger.debug(
                "apply_all_transmog: actor not ready (a1={:#018x}), re-arming in {} ms",
                static_cast<uint64_t>(a1),
                apply_ready_retry_ms
            );
            schedule_transmog_ms(apply_ready_retry_ms);
            return;
        }

        // Suppress VEC hook for the entire operation.

        logger.trace("[dispatch] apply_all_transmog entry a1={:#018x}", static_cast<uint64_t>(a1));

        log_carrier_resolution(a1, PresetManager::instance().active_character());

        // Snapshot last_ids for diagnostic logging.
        const std::array<uint16_t, SLOT_COUNT> prev_ids = last_ids;

        // One-shot per-slot "force apply" snapshot (read-and-clear). Set by the body-mesh picker when the user re-picks
        // a prefab on the same carrier id (the id is unchanged, but the src->tgt wrapper map differs). Without this
        // signal the dispatcher sees `would_be == prev_ids[i]` and skips both preset_changed AND slot_needs_work. Phase
        // A `tear_down_fake` then never runs for the slot, and nothing drives the engine's natural-pipeline hook to
        // clean up the prior tgt wrapper. With the flag set, the dispatcher behaves as if the slot's preset changed,
        // and prev_ids[i] stays intact so Phase A still tears down the prior carrier.
        std::array<bool, SLOT_COUNT> force_apply{};
        {
            auto &fa = force_apply_pending();
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                force_apply[i] = fa[i];
                fa[i] = false;
            }
        }

        // Early-out: skip all work if neither the preset nor the real armor changed since the last successful
        // apply. Drops spurious re-apply cycles fired by BatchEquip/VEC for non-armor events (weapon swaps, ring
        // changes, etc.).
        //
        // live_real_ids is indexed by TransmogSlot enum value (0..SLOT_COUNT-1) and populated by walking
        // TEAR_DOWN_SLOTS which already enumerates every supported slot with its engine tag. Slots LT does not manage
        // stay zeroed.
        std::array<std::uint16_t, SLOT_COUNT> live_real_ids{};
        if (real_part_tear_down::is_ready())
        {
            for (const auto &td : TEAR_DOWN_SLOTS)
            {
                const auto idx = static_cast<std::size_t>(td.slot);
                live_real_ids[idx] = real_part_tear_down::get_real_item_id(reinterpret_cast<void *>(a1), td.game_tag);
            }
        }

        std::array<bool, SLOT_COUNT> slot_needs_work{};
        {
            bool preset_changed = false;
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                const auto &m = mappings[i];
                const uint16_t would_be = (m.active && m.target_item_id != 0) ? m.target_item_id : 0;
                if (would_be != prev_ids[i] || force_apply[i])
                {
                    preset_changed = true;
                    break;
                }
            }

            bool real_changed = false;
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                if (live_real_ids[i] != last_applied_real_ids()[i])
                {
                    real_changed = true;
                    break;
                }
            }

            // Check for active "none" slots - these need Phase B tear-down and suppress reinforcement even when
            // nothing else changed. The game can re-equip real items after the initial tear-down during the load
            // sequence.
            bool has_active_none = false;
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                if (mappings[i].active && mappings[i].target_item_id == 0)
                {
                    has_active_none = true;
                    break;
                }
            }

            if (!preset_changed && !real_changed && !has_active_none)
            {
                char prev_buf[256];
                char real_buf[256];
                format_slot_ids(prev_ids.data(), prev_buf, sizeof(prev_buf));
                format_slot_ids(live_real_ids.data(), real_buf, sizeof(real_buf));
                logger.trace("apply_all_transmog: no state change (prev=[{}] real=[{}]), skipping", prev_buf, real_buf);
                return;
            }

            if (!preset_changed && real_changed)
            {
                char old_buf[256];
                char new_buf[256];
                format_slot_ids(last_applied_real_ids().data(), old_buf, sizeof(old_buf));
                format_slot_ids(live_real_ids.data(), new_buf, sizeof(new_buf));
                logger
                    .debug("apply_all_transmog: real item changed, re-applying (real=[{}] -> [{}])", old_buf, new_buf);

                // Real swap means any previously-damaged slot now has a NEW real item that is NOT damaged yet. Clear
                // the damage flags for slots whose real id changed so the fake==real skip works correctly for the new
                // real.
                for (std::size_t i = 0; i < SLOT_COUNT; ++i)
                {
                    if (live_real_ids[i] != last_applied_real_ids()[i])
                        real_damaged()[i] = false;
                }
            }

            // Per-slot "needs work" flags. Computed BEFORE last_applied_real_ids is overwritten, so the comparison
            // runs against the previous state. A slot needs tear-down + re-apply if its preset target changed OR its
            // underlying real item changed. Unchanged slots are skipped - no tear-down, no cache clear, no
            // SlotPopulator call - so they do not flicker.
            //
            // Unticked slots whose real changes ARE still marked: a prior restore through SlotPopulator can leave a
            // dispatch cache entry and a scene-graph mesh. The cleanup pass after the untick-restore loop relies on
            // slot_needs_work to find and tear down these stale entries.
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                const auto &m = mappings[i];
                const uint16_t would_be = (m.active && m.target_item_id != 0) ? m.target_item_id : uint16_t{0};
                if (would_be != prev_ids[i] || force_apply[i])
                {
                    slot_needs_work[i] = true;
                    continue;
                }

                // Check if the real item changed for this slot. Unticked slots still need cache cleanup when their real
                // changes - an earlier restore through SlotPopulator can leave a dispatch entry that the game's own
                // unequip flow cannot remove. live_real_ids and last_applied_real_ids are both indexed by TransmogSlot
                // (SLOT_COUNT-wide), so the comparison is direct.
                if (live_real_ids[i] != last_applied_real_ids()[i])
                    slot_needs_work[i] = true;
                // Active "none" slots always need work for suppress reinforcement.
                if (m.active && m.target_item_id == 0)
                    slot_needs_work[i] = true;
            }

            // Master enable mask. Disabled slots (multi-prefab non-armor and duplicate-tag slots - see
            // SlotMetadata::enabled doc-block in slot_metadata.hpp) never participate in the dispatch, even if a preset
            // loaded them with active=true. This is the single defensive gate covering preset load, legacy presets
            // saved before disabling, and any future path that toggles `mappings[i].active`.
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                if (!slot_enabled(i))
                    slot_needs_work[i] = false;
            }

            // NOTE: last_applied_real_ids is updated at the END of the function, after all applies succeed. On a
            // mid-apply fault (e.g. reload SEH) the old values remain, so the next retry detects the real-armor change
            // and tries again.
        }

        // Clear last_ids for slots without a new target.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            auto &m = mappings[i];
            if (!m.active || m.target_item_id == 0)
                last_ids[i] = 0;
        }

        // SlotPopulator maintains a dispatch cache on the component at (basePtr, count, cap). The COMP_SLOT_CACHE_*
        // constants at the top of this file carry the triple's offsets. Each entry is 24 bytes:
        //   +0x00 uint16  slotNativeId
        //   +0x08 __int128* subArray (queued ItemInfoBlobs)
        //   +0x10 uint32  subCount   (append-only, NEVER reset by game)
        //   +0x14 uint32  subCap
        //
        // Two hazards:
        //  (1) Stale entries survive across presets. A save and restore of the count re-exposes previous-preset
        //      entries. SlotPopulator's linear search then finds an old slotNativeId and *appends* a new blob to its
        //      existing subArray. The engine still dispatches the stale blob to VEC, so the previous preset's helm
        //      stays on screen.
        //  (2) Even on a slot this pass DOES re-populate, the game never clears subCount, so the previously queued
        //      blob lingers and replays alongside the new one.
        //
        // Fix: only clear subCount for dispatch cache entries whose slotNativeId matches a slot this pass re-applies.
        // A blanket clear nukes unchanged slots' blobs, which forces the game to re-dispatch them through VEC and
        // makes unchanged gear flicker.
        //
        // Build a set of game tags that need clearing: any active slot with a non-zero target, plus any unticked slot
        // that needs real-item restoration.
        std::uint16_t clear_tags[SLOT_COUNT]{};
        std::size_t clear_tag_count = 0;
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (slot_needs_work[i])
                clear_tags[clear_tag_count++] = static_cast<std::uint16_t>(SLOT_METADATA[i].game_tag);
        }

        __try
        {
            const auto count = *reinterpret_cast<volatile uint32_t *>(a1 + COMP_SLOT_CACHE_COUNT_OFFSET);
            const auto base = *reinterpret_cast<volatile uintptr_t *>(a1 + COMP_SLOT_CACHE_BASE_PTR_OFFSET);
            if (plausible_engine_ptr(static_cast<__int64>(base)))
            {
                for (uint32_t e = 0; e < count; ++e)
                {
                    const auto entry = base + 24ULL * e;
                    const auto slot_id = *reinterpret_cast<volatile uint16_t *>(entry);
                    for (std::size_t t = 0; t < clear_tag_count; ++t)
                    {
                        if (slot_id == clear_tags[t])
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
        // Phase A - tear down the previous preset's fake meshes, with last_ids[] as the item_id source.
        // Phase B - tear down the REAL item in the auth table for every active slot that needs work, even when the
        //   new fake equals the real item.
        //
        // Both phases go through the engine's part-detach entry point, which detaches particle emitters and anim
        // controllers from the scene graph. The auth table is NOT mutated.
        //
        // Game slot tags:
        //   Helm=0x03 Chest=0x04 Gloves=0x05 Boots=0x06 Cloak=0x10.
        // The TearDownSlot struct + TEAR_DOWN_SLOTS array are defined at file scope above so the dispatcher entry block
        // can also walk them when reading live_real_ids. TEAR_DOWN_COUNT is available from the same scope.

        // Snapshot the real equipped item_id for each slot up front so both phases and the part_show_suppress mask can
        // compare without re-walking the auth table.
        std::uint16_t real_item_id[TEAR_DOWN_COUNT]{};
        if (real_part_tear_down::is_ready())
        {
            for (std::size_t k = 0; k < TEAR_DOWN_COUNT; ++k)
            {
                real_item_id[k] =
                    real_part_tear_down::get_real_item_id(reinterpret_cast<void *>(a1), TEAR_DOWN_SLOTS[k].game_tag);
            }

            // Phase A: previous fakes, taken from last_ids as it stood before this apply.
            for (std::size_t k = 0; k < TEAR_DOWN_COUNT; ++k)
            {
                const auto &td = TEAR_DOWN_SLOTS[k];
                const auto idx = static_cast<std::size_t>(td.slot);
                if (!slot_needs_work[idx])
                    continue;
                const auto prev_id = prev_ids[idx];
                const auto prev_carrier = last_applied_carrier_ids()[idx];
                if (prev_id == 0)
                {
                    // First-claim hide: active-none slot LT never owned (prev_ids==0, no carrier history). Phase B
                    // alone calls the scene-graph tear-down once, and one call does not detach the part for a slot
                    // where LT never placed a carrier (e.g. Mask/Necklace on the first apply of an all-none preset).
                    // The doubled call here matches the working manual path (transmog-something -> none), which fires
                    // Phase A on the prior carrier plus Phase B on the real entry - same hash, same slot tag, twice.
                    const auto &m = mappings[idx];
                    if (m.active && m.target_item_id == 0 && live_real_ids[idx] != 0)
                    {
                        logger.trace(
                            "[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} (first-claim hide)",
                            td.game_tag,
                            static_cast<std::uint16_t>(live_real_ids[idx])
                        );
                        real_part_tear_down::tear_down_by_item_id(
                            reinterpret_cast<void *>(a1),
                            live_real_ids[idx],
                            td.game_tag
                        );
                    }
                    continue;
                }

                // Phase A runs unconditionally: fake and real get equal treatment, so a previous fake that matches the
                // live real item is still torn down.
                if (prev_carrier != 0 && prev_carrier != static_cast<std::uint16_t>(prev_id))
                {
                    logger.trace(
                        "[dispatch] tear_down_fake slot={:#06x} carrier={:#06x} (then target={:#06x})",
                        td.game_tag,
                        prev_carrier,
                        static_cast<std::uint16_t>(prev_id)
                    );
                    real_part_tear_down::tear_down_by_item_id(reinterpret_cast<void *>(a1), prev_carrier, td.game_tag);
                }
                real_part_tear_down::tear_down_by_item_id(
                    reinterpret_cast<void *>(a1),
                    static_cast<std::uint16_t>(prev_id),
                    td.game_tag
                );

                // Direct-applied fake (Mask/Necklace, or any carrier==target collapse) with no matching real
                // underneath: give it the extra detach its rendered per-body rig needs to come off. No-op for
                // distinct-carrier items and when Phase B re-tears the same hash (live real == fake). See
                // tear_down_direct_fake_second_pass.
                tear_down_direct_fake_second_pass(
                    a1,
                    static_cast<std::uint16_t>(prev_id),
                    static_cast<std::uint16_t>(td.game_tag),
                    real_item_id[k],
                    prev_carrier != 0 && prev_carrier != static_cast<std::uint16_t>(prev_id)
                );
            }

            // Phase B: real items for any active slot. Runs unconditionally - fake and real get equal treatment, so
            // a new fake that matches the live real still tears down the real part. Only slots without a
            // slot_needs_work flag, and inactive slots, are skipped.
            for (std::size_t k = 0; k < TEAR_DOWN_COUNT; ++k)
            {
                const auto &td = TEAR_DOWN_SLOTS[k];
                const auto idx = static_cast<std::size_t>(td.slot);
                if (!slot_needs_work[idx])
                    continue;
                auto &m = mappings[idx];
                if (!m.active)
                    continue;
                if (real_part_tear_down::tear_down_real_part(reinterpret_cast<void *>(a1), td.game_tag))
                {
                    real_damaged()[idx] = true;
                }
            }
        }

        // Detect preset-switch AFTER tear-down completes but BEFORE the per-slot apply loop. If the new gear differs
        // from the gear active at the last swap apply, deactivate so the upcoming apply loop's substitutions do not
        // re-bind target wrappers to the new gear. (The reverse-write of prior tracked structs already ran
        // pre-tear-down, so the engine can unlink them during tear-down.) Order: Helm/Chest/Cloak/Gloves/Boots - the
        // fixed 5-armor order of the prefab-wrapper-swap notify_apply_starting contract.
        {
            const std::uint16_t new_items[5] = {
                static_cast<std::uint16_t>(
                    mappings[static_cast<std::size_t>(TransmogSlot::Helm)].active
                        ? mappings[static_cast<std::size_t>(TransmogSlot::Helm)].target_item_id
                        : 0
                ),
                static_cast<std::uint16_t>(
                    mappings[static_cast<std::size_t>(TransmogSlot::Chest)].active
                        ? mappings[static_cast<std::size_t>(TransmogSlot::Chest)].target_item_id
                        : 0
                ),
                static_cast<std::uint16_t>(
                    mappings[static_cast<std::size_t>(TransmogSlot::Cloak)].active
                        ? mappings[static_cast<std::size_t>(TransmogSlot::Cloak)].target_item_id
                        : 0
                ),
                static_cast<std::uint16_t>(
                    mappings[static_cast<std::size_t>(TransmogSlot::Gloves)].active
                        ? mappings[static_cast<std::size_t>(TransmogSlot::Gloves)].target_item_id
                        : 0
                ),
                static_cast<std::uint16_t>(
                    mappings[static_cast<std::size_t>(TransmogSlot::Boots)].active
                        ? mappings[static_cast<std::size_t>(TransmogSlot::Boots)].target_item_id
                        : 0
                ),
            };
            prefab_wrapper_swap::notify_apply_starting(new_items);
        }

        // Helper: look up a slot's real item_id from the snapshot taken during the tear-down phase.
        auto lookup_real_id = [&](std::size_t slot_idx) -> std::uint16_t
        {
            for (std::size_t k = 0; k < TEAR_DOWN_COUNT; ++k)
            {
                if (static_cast<std::size_t>(TEAR_DOWN_SLOTS[k].slot) == slot_idx)
                    return real_item_id[k];
            }
            return 0;
        };

        uint32_t our_written_count = 0;
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            auto &m = mappings[i];
            if (!slot_needs_work[i])
            {
                // Unchanged slot: preserve its last_ids entry but do not re-apply. If it has a live dispatch cache
                // entry, the game keeps rendering it.
                if (m.active && m.target_item_id != 0)
                    last_ids[i] = m.target_item_id;
                continue;
            }
            if (!m.active || m.target_item_id == 0)
                continue;

            // Fake and real get equal treatment: SlotPopulator runs unconditionally, even when the new fake item_id
            // matches the intact live real item. Do NOT add a skip-on-match fast path. The apply sequence must stay
            // one predictable path, and the CPU it saves does not pay for the divergence between matched and
            // unmatched re-applies.
            //
            // Decide: direct apply or carrier-assisted apply. current_apply_owner picks the editing character when the
            // dropdown pin is engaged, so a targeted-apply on a non-controlled body installs THAT body's carrier
            // family. See the matching block in apply_single_slot_transmog for the cross-talk failure mode this
            // avoids.
            const auto tm_slot = static_cast<TransmogSlot>(i);
            const auto targetId = m.target_item_id;
            const auto &active_char = current_apply_owner();
            // Every slot goes through a carrier now. The carrier supplies a legitimately equippable item and the
            // prefab swap supplies the visual. No "can the wearer equip the target" question remains to branch on.
            const uint16_t carrier_id = default_carrier_for_slot(tm_slot, active_char);
            const bool use_carrier = carrier_id != 0;

            // Feed the active preset's per-slot dye state into the record injector. Its inline detour on the engine's
            // dye-publish function appends fabricated ARMOR_MOD records to the publish vector post-trampoline. The
            // injection is independent of any real item the user wears.
            //
            // Bytes +7/+8/+9 of each record carry the literal RGB of the chosen shade. Channels with group_hash == 0
            // are inactive: the injector substitutes the first active channel's settings rather than skipping (sparse
            // injection lets the engine's natural records dominate and the dye does not render).
            publish_preset_dye_for_slot(i);

            color_override::setter_substitute::set_active_slot(static_cast<int>(i));
            // See call site #1 (apply_single_slot) for the rationale. Pass user-intent (active ? user-chosen target :
            // 0) so an untick does not trigger a target-change wipe of seeded placeholders.
            {
                auto &mapping = slot_mappings()[i];
                const std::uint32_t user_intent =
                    mapping.active ? static_cast<std::uint32_t>(mapping.target_item_id) : 0u;
                color_override::reinit::notify_transmog_target(static_cast<int>(i), user_intent);
            }
            if (use_carrier && carrier_id != 0)
            {
                logger.debug(
                    "Transmog APPLY (carrier): slot={}, target={:#06x}, carrier={:#06x}",
                    slot_name(tm_slot),
                    targetId,
                    carrier_id
                );
                logger
                    .trace("[dispatch] applying slot={} targetId={:#06x} via carrier={:#06x}", i, targetId, carrier_id);
                apply_transmog_with_carrier(
                    a1,
                    carrier_id,
                    targetId,
                    slot_needs_explicit_destination(tm_slot) ? static_cast<uint16_t>(game_slot_from_transmog(tm_slot))
                                                             : NO_GAME_TAG,
                    paired_first_half_tag(tm_slot)
                );
            }
            else
            {
                if (use_carrier)
                    logger.warning(
                        "Transmog APPLY: slot={} needs carrier but none resolved, falling back to direct",
                        slot_name(tm_slot)
                    );
                logger.debug("Transmog APPLY: slot={}, target={:#06x}", slot_name(tm_slot), targetId);
                logger.trace("[dispatch] applying slot={} itemId={:#06x}", i, targetId);
                apply_transmog(a1, targetId);
            }

            dye_record_inject::clear_slot_dye_state();
            last_ids[i] = m.target_item_id;
            last_applied_carrier_ids()[i] = (use_carrier && carrier_id != 0) ? carrier_id : 0;
            ++our_written_count;
        }

        // When a slot's checkbox is UNTICKED (!m.active), LT controlled the slot before and Phase B tore down the real
        // item. Restore the real item so it reappears. "Active + none" (checkbox ticked, dropdown=none) means "show
        // empty" - do NOT restore.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            const auto &m = mappings[i];
            if (!m.active && (prev_ids[i] != 0 || real_damaged()[i]))
            {
                const std::uint16_t real_id = lookup_real_id(i);
                if (real_id != 0)
                {
                    logger.info(
                        "[dispatch] slot={} unticked - restoring real item {:#06x}",
                        slot_name(static_cast<TransmogSlot>(i)),
                        real_id
                    );
                    // Snapshot the live dye records on this slot's auth-table entry and publish via the inject channel
                    // so apply_transmog repaints the restored real item in the user's actual inventory dye instead of
                    // the item's factory palette. Same pattern as Pass B in clear_all_transmog.
                    //
                    // Do NOT mirror this live dye into the active preset here. The `!m.active` gate above is shared
                    // with mass-reset paths the user never triggers directly (mod-disable at the top of this same
                    // function, character switch, Unpin, preset load, color_override re-init). A mirror here silently
                    // bakes the real item's dye into the preset on every toggle-off and every character switch.
                    // Real-dye capture into the preset stays explicit: Capture Outfit (mass), or the per-slot "Sync
                    // from live" button in the dye popup.
                    const auto game_tag = game_slot_from_transmog(static_cast<TransmogSlot>(i));
                    if (!publish_entry_dye_for_gameslot(a1, game_tag))
                        dye_record_inject::clear_slot_dye_state();
                    color_override::setter_substitute::set_active_slot(static_cast<int>(i));
                    apply_transmog(a1, real_id);
                    dye_record_inject::clear_slot_dye_state();
                    ++our_written_count;
                }
                else if (real_damaged()[i])
                {
                    // The real item was unequipped (real_id=0), but an earlier restore through SlotPopulator left a
                    // scene-graph mesh entry. Tear it down so the visual disappears. The old real ID is still in
                    // last_applied_real_ids, which this function overwrites only at its end.
                    for (std::size_t k = 0; k < TEAR_DOWN_COUNT; ++k)
                    {
                        if (static_cast<std::size_t>(TEAR_DOWN_SLOTS[k].slot) != i)
                            continue;
                        // last_applied_real_ids is TransmogSlot-indexed, so look up by slot enum (== i), not by k.
                        const auto old_real = last_applied_real_ids()[i];
                        if (old_real != 0)
                        {
                            logger.info(
                                "[dispatch] slot={} unticked + unequipped - tearing down restored mesh {:#06x}",
                                slot_name(static_cast<TransmogSlot>(i)),
                                old_real
                            );
                            real_part_tear_down::tear_down_by_item_id(
                                reinterpret_cast<void *>(a1),
                                old_real,
                                TEAR_DOWN_SLOTS[k].game_tag
                            );
                        }
                        break;
                    }
                }
                // Clear damage flag so this slot is fully released back to the game. Without this, future apply cycles
                // keep treating it as managed.
                real_damaged()[i] = false;
            }
            if (!m.active || m.target_item_id == 0)
                last_applied_carrier_ids()[i] = 0;
        }

        // Cleanup pass: tear down stale scene-graph meshes left by a prior restore through SlotPopulator. This handles
        // the case where apply_single_slot restored the real item and created a scene-graph entry, and the user then
        // unequipped it in the game inventory. The untick-restore block above does not catch that case, because
        // apply_single_slot already cleared prev_ids and real_damaged. The signature detected here is slot_needs_work +
        // unticked + real=0 + an old real in last_applied_real_ids, which is not overwritten yet.
        for (std::size_t k = 0; k < TEAR_DOWN_COUNT; ++k)
        {
            const auto &td = TEAR_DOWN_SLOTS[k];
            const auto idx = static_cast<std::size_t>(td.slot);
            if (!slot_needs_work[idx])
                continue;
            if (mappings[idx].active)
                continue;
            if (real_item_id[k] != 0)
                continue;
            // last_applied_real_ids is TransmogSlot-indexed - look up by `idx` (the slot enum), not the iteration
            // counter.
            const auto old_real = last_applied_real_ids()[idx];
            if (old_real == 0)
                continue;
            logger.info(
                "[dispatch] slot={} cleanup - tearing down stale restore mesh {:#06x}",
                slot_name(td.slot),
                old_real
            );
            real_part_tear_down::tear_down_by_item_id(reinterpret_cast<void *>(a1), old_real, td.game_tag);
        }

        // Count was NOT zeroed - unchanged slots' entries are still live with their original subCount. Log the final
        // state for diagnostics.
        __try
        {
            uint32_t live_count = *reinterpret_cast<volatile uint32_t *>(a1 + COMP_SLOT_CACHE_COUNT_OFFSET);
            logger.trace("[dispatch] post-apply liveCount={} ourWrittenCount={}", live_count, our_written_count);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        // target_mask: slots with a fake mesh to render. active_mask: slots the user explicitly controls.
        std::uint32_t target_mask = 0;
        std::uint32_t active_mask = 0;
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            if (last_ids[i] != 0)
                target_mask |= (std::uint32_t{1} << i);
            if (mappings[i].active)
                active_mask |= (std::uint32_t{1} << i);
        }

        // Suppression rule: suppress every active slot whose real item is torn down, or will be. If the fake item_id
        // matches the real equipped id, the real item is still live and LT must NOT suppress it.
        std::uint32_t suppress_mask = 0;
        for (std::size_t k = 0; k < TEAR_DOWN_COUNT; ++k)
        {
            const auto idx = static_cast<std::size_t>(TEAR_DOWN_SLOTS[k].slot);
            const auto &m = mappings[idx];
            if (!m.active)
                continue;
            if (m.target_item_id != 0 && static_cast<std::uint16_t>(m.target_item_id) == real_item_id[k])
                continue;
            suppress_mask |= (std::uint32_t{1} << idx);
        }

        {
            // Split prev and now across two log lines so each fits a normal terminal.
            char prev_buf[256];
            char now_buf[256];
            format_slot_ids(prev_ids.data(), prev_buf, sizeof(prev_buf));
            format_slot_ids(last_ids.data(), now_buf, sizeof(now_buf));
            logger.info("apply_all_transmog prev=[{}]", prev_buf);
            logger.info(
                "apply_all_transmog now=[{}] target={:#x} active={:#x} suppress={:#x}",
                now_buf,
                target_mask,
                active_mask,
                suppress_mask
            );
        }

        part_show_suppress::set_mask(static_cast<uint32_t>(suppress_mask));

        // Commit the real-armor snapshot AFTER all applies succeed. On a fault (SEH during reload) this line is never
        // reached, and the next retry detects the real-armor change correctly.
        last_applied_real_ids() = live_real_ids;

        // Record this apply's itemIds with body-mesh pointer swap so the next apply can detect a preset-switch and
        // auto-deactivate.
        {
            const std::uint16_t applied_items[5] = {
                static_cast<std::uint16_t>(last_ids[static_cast<std::size_t>(TransmogSlot::Helm)]),
                static_cast<std::uint16_t>(last_ids[static_cast<std::size_t>(TransmogSlot::Chest)]),
                static_cast<std::uint16_t>(last_ids[static_cast<std::size_t>(TransmogSlot::Cloak)]),
                static_cast<std::uint16_t>(last_ids[static_cast<std::size_t>(TransmogSlot::Gloves)]),
                static_cast<std::uint16_t>(last_ids[static_cast<std::size_t>(TransmogSlot::Boots)]),
            };
            prefab_wrapper_swap::notify_apply_finished(applied_items);

            // Rebuild the slots that REPLACED an earlier LT target.
            //
            // The sweep above erases the previous target's CLAIM, and the claim count does drop - but the part is
            // already realized, and dropping a claim does not retract what is on screen. The engine only reconciles
            // on a rebuild, so the stale mesh survives until one happens.
            //
            // This is what lets a target change stay instant: the new visual installs, the stale claim is erased,
            // and the slot rebuilds against the claims that remain - no tear-down anywhere in the path.
            //
            // Both conditions must hold. A wider condition rebuilds every slot every pass at no benefit.
            //
            // - `prev_ids[i] != 0` - an earlier LT target must exist to retract. On the first apply after a world load
            //   the slot held the REAL item, which Phase A/B already tore down, so nothing is stale.
            // - `prev_ids[i] != target_item_id` - measured against the entry snapshot, NOT against `last_ids`.
            // `last_ids`
            //   IS last_applied_ids(), which the apply loop above already filled with this apply's targets, so every
            //   slot compares equal there and no slot ever rebuilds.
            //
            // The rebuild goes through refresh_slot_appearance, NEVER the bare refresh_slot_visual. The apply loop
            // clears the dye state after each slot, so a bare rebuild drives a DyeCopier call with nothing published
            // and the engine re-emits its natural records, which silently strips the color the apply injected.
            // refresh_slot_appearance republishes this slot's dye and rebinds its color_override slot around the
            // rebuild, exactly as the single-slot path does.
            for (std::size_t i = 0; i < SLOT_COUNT; ++i)
            {
                const auto sl = static_cast<TransmogSlot>(i);
                if (!slot_enabled(sl) || !mappings[i].active || mappings[i].target_item_id == 0)
                    continue;
                if (prev_ids[i] == 0 || prev_ids[i] == mappings[i].target_item_id)
                    continue; // no earlier target, or unchanged - nothing stale to reconcile
                refresh_slot_appearance(i);
            }
        }

        // prefab_wrapper_swap stays active across applies. Do NOT schedule an auto-deactivate after each apply. The
        // wrapper-substitution path has no cheap teardown - a heap walk on deactivate stalls preset switches by about
        // a minute. The residual helm leak also sits outside that path: it lives in scene-graph children that the
        // engine re-parents through its runtime-resource-pointer keying. Clearing that residue needs PAZ-level
        // patching. Users press LT's Clear button when they want the swap torn down explicitly.
    }

    void clear_all_transmog(__int64 a1)
    {
        auto &logger = DMK::log();
        auto &last_ids = last_applied_ids();

        // Fallback only - see apply_single_slot_transmog comment.
        if (!plausible_engine_ptr(a1) && world_system_ptr().load(std::memory_order_acquire))
        {
            const auto fresh = resolve_player_component();
            if (plausible_engine_ptr(fresh))
                a1 = fresh;
        }

        // Snapshot the previously applied fakes BEFORE clearing last_ids. Iteration order across `SLOT_METADATA` is
        // irrelevant for correctness: prev_fake_id / prev_carrier_id are indexed by `k` (the array slot), and the
        // per-slot snapshot reads `last_ids` by `slot` (the TransmogSlot enum value). The engine-only tag 0x0015 is
        // absent from `SLOT_METADATA` by design (see `TransmogSlot` enum in `shared_state.hpp`), so the loop skips it
        // automatically.
        std::uint16_t prev_fake_id[SLOT_COUNT]{};
        for (std::size_t k = 0; k < SLOT_COUNT; ++k)
        {
            const auto idx = static_cast<std::size_t>(SLOT_METADATA[k].slot);
            prev_fake_id[k] = static_cast<std::uint16_t>(last_ids[idx]);
        }

        // Snapshot carrier IDs before clearing.
        std::uint16_t prev_carrier_id[SLOT_COUNT]{};
        for (std::size_t k = 0; k < SLOT_COUNT; ++k)
        {
            const auto idx = static_cast<std::size_t>(SLOT_METADATA[k].slot);
            prev_carrier_id[k] = last_applied_carrier_ids()[idx];
        }

        // Clear last_ids, carrier IDs, and per-slot damage flags.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            last_ids[i] = 0;
            last_applied_carrier_ids()[i] = 0;
            real_damaged()[i] = false;
        }

        // Pass A: tear down orphan fakes.
        if (real_part_tear_down::is_ready())
        {
            for (std::size_t k = 0; k < SLOT_COUNT; ++k)
            {
                const auto game_tag = static_cast<std::uint16_t>(SLOT_METADATA[k].game_tag);
                const auto fake_id = prev_fake_id[k];
                const auto c_id = prev_carrier_id[k];
                if (fake_id == 0)
                    continue;
                const auto real_id = real_part_tear_down::get_real_item_id(reinterpret_cast<void *>(a1), game_tag);
                if (real_id == fake_id && c_id == 0)
                {
                    logger.trace(
                        "[clear] orphan-check slot={:#06x} fake={:#06x} skipped (matches real, no carrier)",
                        game_tag,
                        fake_id
                    );
                    continue;
                }

                // Tear the carrier's own identity down first when it is a distinct item.
                if (c_id != 0 && c_id != fake_id)
                {
                    logger.info(
                        "[clear] tearing carrier slot={:#06x} carrier={:#06x} (real={:#06x})",
                        game_tag,
                        c_id,
                        real_id
                    );
                    real_part_tear_down::tear_down_by_item_id(reinterpret_cast<void *>(a1), c_id, game_tag);
                }
                logger.info(
                    "[clear] tearing orphan fake slot={:#06x} itemId={:#06x} (real={:#06x} carrier={:#06x})",
                    game_tag,
                    fake_id,
                    real_id,
                    c_id
                );
                real_part_tear_down::tear_down_by_item_id(reinterpret_cast<void *>(a1), fake_id, game_tag);

                // Direct-applied orphan fake with no matching real underneath: it needs a second detach.
                // See tear_down_direct_fake_second_pass.
                tear_down_direct_fake_second_pass(a1, fake_id, game_tag, real_id, c_id != 0 && c_id != fake_id);
            }
        }
        else
        {
            logger.debug("[clear] RealPartTearDown not ready - pass A skipped");
        }

        // Reset the real-item snapshot so the next apply re-reads from the live auth table.
        last_applied_real_ids().fill(0);

        __try
        {
            auto entry_desc = *reinterpret_cast<uintptr_t *>(a1 + auth_table::CONTAINER_PTR_OFFSET);
            if (plausible_engine_ptr(static_cast<__int64>(entry_desc)))
            {
                auto entry_array = *reinterpret_cast<uintptr_t *>(entry_desc + auth_table::CONTAINER_ARRAY_BASE_OFFSET);
                auto entryCount = *reinterpret_cast<uint32_t *>(entry_desc + auth_table::CONTAINER_COUNT_OFFSET);

                auto saved_count = *reinterpret_cast<uint32_t *>(a1 + COMP_SLOT_CACHE_COUNT_OFFSET);
                *reinterpret_cast<uint32_t *>(a1 + COMP_SLOT_CACHE_COUNT_OFFSET) = 0;

                for (uint32_t e = 0; e < entryCount && plausible_engine_ptr(static_cast<__int64>(entry_array)); ++e)
                {
                    auto base = entry_array + e * auth_table::ENTRY_STRIDE;
                    auto game_slot = *reinterpret_cast<int16_t *>(base + auth_table::ENTRY_SLOT_TAG_OFFSET);
                    auto item_id = *reinterpret_cast<uint16_t *>(base + auth_table::ENTRY_ITEM_ID_OFFSET);

                    if (item_id == 0 || item_id == 0xFFFF)
                        continue;

                    auto tm_slot = slot_from_game_slot(game_slot);
                    if (!tm_slot.has_value())
                        continue;

                    // Snapshot the equipped item's live dye records and re-publish them through the inject channel, so
                    // the following apply_transmog -> SlotPopulator -> DyeCopier round-trip emits them into the render
                    // struct's dst+120. Without this, the synthesized swap_entry passes through DyeCopier empty. The
                    // engine then resolves the slot to its factory palette and paints toggled-off items un-dyed.
                    dye_record_inject::ChannelState live_dye[dye_record_inject::DYE_CHANNEL_COUNT];
                    if (dye_record_inject::read_entry_dye_records(base, live_dye) > 0)
                    {
                        dye_record_inject::log_dye_snapshot("restore", slot_name(*tm_slot), live_dye);
                        // sparse: mirror the source channels exactly, so the pass does not paint mesh parts that the
                        // real item never colored.
                        dye_record_inject::set_slot_dye_state(live_dye, /*sparse=*/true);
                    }
                    else
                    {
                        dye_record_inject::clear_slot_dye_state();
                    }

                    logger.debug("Transmog RESTORE: real item {:#06x} for slot {}", item_id, game_slot_name(game_slot));
                    apply_transmog(a1, item_id);

                    dye_record_inject::clear_slot_dye_state();
                }

                // Restore count to the larger of saved and live.
                uint32_t live_count = *reinterpret_cast<volatile uint32_t *>(a1 + COMP_SLOT_CACHE_COUNT_OFFSET);
                uint32_t final_count = (live_count > saved_count) ? live_count : saved_count;
                *reinterpret_cast<volatile uint32_t *>(a1 + COMP_SLOT_CACHE_COUNT_OFFSET) = final_count;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.warning("Transmog clear exception during restore");
        }

        part_show_suppress::clear_all_suppressed();

        // Intentionally do NOT call prefab_wrapper_swap::deactivate_for_clear() here.
        //
        // The natpipe hook must stay armed (s_active=true with s_swap_map_per_char populated) so any later organic
        // unequip or scene-graph teardown - for example when the user swaps gear through the radial after a Clear -
        // can still find and unlink the Bastier-target wrappers LT installed in parent+88. If the hook is disarmed
        // here, the engine searches with Kliff src wrappers, misses the Bastier targets, and leaks ghost meshes. That
        // ghost-helm leak is the reason this hook exists.
        //
        // The on_struct_copy hook is independently silenced after toggle-off by its in_transmog() gate, so leaving the
        // swap map armed only matters during the engine's own cleanup walks, which is exactly when it must fire.

        logger.info("Transmog CLEAR: done");
    }

} // namespace Transmog
