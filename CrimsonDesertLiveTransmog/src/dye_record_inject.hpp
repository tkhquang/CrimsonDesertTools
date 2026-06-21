#pragma once

// Per-slot dye-record injector for fake transmog.
//
// LT-fake transmog items have no real-item backing, so the engine has no natural ARMOR_MOD records to publish for them.
// Without records, the slot renders with the engine's default palette regardless of the user's preset.
//
// This module installs an inline detour on the engine's per-slot dye-record copier (`sub_141E019E0`, "DyeCopier").
// Post-trampoline, when LT is active and a per-slot dye state has been published, the detour calls the engine's own
// 16-byte record-copy primitive (`sub_140CADEF0`, "DyeCopy") to APPEND fabricated ARMOR_MOD records to the destination
// vector at `dst+120`.
//
// Each fabricated record encodes one channel's RGB at offsets +7/+8/+9. Channels with `group_hash == 0` are inactive:
// they reuse the first active channel's settings so the engine sees a contiguous record block. Sparse injection
// (skipping inactive channels) leaves the engine's natural records dominant and the dye does not render.
//
// Function targets are resolved at init via patch-proof AOB cascades in `aob_resolver.hpp`; raw RVAs are not used as
// anchors.

#include <cstddef>
#include <cstdint>

namespace Transmog::DyeRecordInject
{
    /**
     * Maximum dye channels per slot. Mirrors PresetSlot's `k_dyeChannelCount` but is independent so this header does
     * not pull in the preset types.
     */
    inline constexpr std::size_t k_dyeChannelCount = 16;

    /**
     * One channel's dye state. `group_hash == 0` means "no override for this channel"; the injector substitutes the
     * first active channel's settings (see header preamble).
     *
     * Field layout in the 16-byte ARMOR_MOD record we synthesize:
     *   +0..+3   group_hash      (color group key)
     *   +4..+5   material_id     (dye-template variant 1..10;
     *                            0xFFFF = engine default)
     *   +6       channel index   (0..N-1; written at inject time)
     *   +7       r
     *   +8       g
     *   +9       b
     *   +10      0xFF
     *   +11      repair_byte     (0=pristine, 0x7F=max wear; the
     *                            engine still accepts 0xFF as a legacy "no override" sentinel and renders it as
     *                            pristine)
     *   +13      0x04 for indices 0 and 3 (mirrors natural records)
     */
    struct ChannelState
    {
        std::uint32_t group_hash;
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
        std::uint16_t material_id;
        std::uint8_t repair_byte;
    };

    /**
     * Resolve target functions via AOB cascades and install hooks. Returns true on success. Failure to resolve any
     * required target disables injection (logged as a warning); the rest of LT continues to function.
     */
    bool init() noexcept;

    /// Diagnostic: log SlotPopulator observation counters.
    void log_counters() noexcept;

    /**
     * Legacy alias; SlotPopulator hook was retired but the call site in `prefab_wrapper_swap.cpp` still invokes this on
     * LT deactivation. Kept as a no-op stat dump.
     */
    void restore_all() noexcept;

    /**
     * Publish per-slot dye state. The detour reads this on the next DyeCopier invocation. `channels` MUST be exactly
     * `k_dyeChannelCount` entries; thread-local because the detour runs on the same thread that publishes.
     *
     * `sparse` toggles the per-record emission strategy:
     * - `false` (default, fake-transmog path): always emit `k_dyeChannelCount` records; channels with `group_hash == 0`
     *   reuse the first active channel's settings so the engine sees a contiguous block. Required for LT-fake meshes
     *   because their natural source has zero records and a sparse vector leaves the engine on its default palette.
     * - `true` (restore path): emit ONLY records whose source had `group_hash != 0`. Inactive channels stay absent from
     *   the destination vector so the engine doesn't paint mesh parts that the real item never colored. Use this when
     *   the `ChannelState` array was sourced from a real auth-table entry's dye vector and you want the render side to
     *   mirror exactly the same indices.
     */
    void set_slot_dye_state(const ChannelState *channels, bool sparse = false) noexcept;

    /**
     * Clear the published state. Call after the apply path completes so subsequent natural equip events go un-modified.
     */
    void clear_slot_dye_state() noexcept;

    /**
     * Read the live dye-record vector from an auth-table entry into `out`, indexed by the in-record channel index
     * (0..N-1). Inactive or out-of-range channels are left zero-initialised.
     *
     * Source layout:
     *   entryBase + 0x78  qword  data_ptr  -- contiguous 16-byte records
     *   entryBase + 0x80  dword  count
     *   entryBase + 0x84  dword  capacity  -- ignored
     * Each record is the same shape this header documents above (the engine's ARMOR_MOD format that DyeCopy emits).
     *
     * Returns the number of channels populated. `0` means nothing usable -- either the vector is empty, the data
     * pointer is invalid, or every record had `group_hash == 0`.
     *
     * Caller is responsible for SEH-protecting the call when `entryBase` is not known to be valid; the function does no
     * page-fault guarding of its own.
     */
    std::size_t read_entry_dye_records(std::uintptr_t entryBase, ChannelState (&out)[k_dyeChannelCount]) noexcept;

    /**
     * Trace-log a ChannelState array using a stable per-channel format so capture-time and apply-time snapshots can be
     * diffed visually in the log. `source` is a short tag like "capture" or "restore"; `slotName` identifies which LT
     * slot the records belong to. Channels with `group_hash == 0` are included so the diff catches missing channels
     * too.
     */
    void log_dye_snapshot(const char *source, const char *slotName,
                          const ChannelState (&state)[k_dyeChannelCount]) noexcept;

    /**
     * Read the first ACTIVE channel's RGB from the currently published slot dye state. Returns true if the slot has any
     * active channel and `r/g/b` are filled; false if no inject is active or all channels are zero. Used by
     * `ColorOverride::SetterSubstitute` to know what color to redirect the engine's per-property setter to.
     */
    bool get_published_first_active_rgb(std::uint8_t *r, std::uint8_t *g, std::uint8_t *b) noexcept;
} // namespace Transmog::DyeRecordInject
