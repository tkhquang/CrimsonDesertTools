#pragma once

// Per-slot dye-record injector for fake transmog.
//
// LT-fake transmog items have no real-item backing, so the engine has
// no natural ARMOR_MOD records to publish for them. Without records,
// the slot renders with the engine's default palette regardless of
// the user's preset.
//
// This module installs an inline detour on the engine's per-slot
// dye-record copier (`sub_141E019E0`, "DyeCopier"). Post-trampoline,
// when LT is active and a per-slot dye state has been published, the
// detour calls the engine's own 16-byte record-copy primitive
// (`sub_140CADEF0`, "DyeCopy") to APPEND fabricated ARMOR_MOD records
// to the destination vector at `dst+120`.
//
// Each fabricated record encodes one channel's RGB at offsets +7/+8/+9
// (cracked 2026-05-10 PM). Channels with `group_hash == 0` are
// inactive: they reuse the first active channel's settings so the
// engine sees a contiguous record block. Sparse injection (skipping
// inactive channels) leaves the engine's natural records dominant and
// the dye does not render.
//
// Function targets are resolved at init via patch-proof AOB cascades
// in `aob_resolver.hpp`; raw RVAs are not used as anchors.

#include <cstddef>
#include <cstdint>

namespace Transmog::DyeRecordInject
{
    /// Maximum dye channels per slot. Mirrors PresetSlot's
    /// `k_dyeChannelCount` but is independent so this header does
    /// not pull in the preset types.
    inline constexpr std::size_t k_dyeChannelCount = 16;

    /// One channel's dye state. `group_hash == 0` means "no override
    /// for this channel"; the injector substitutes the first active
    /// channel's settings (see header preamble).
    ///
    /// Field layout in the 16-byte ARMOR_MOD record we synthesize:
    ///   +0..+3   group_hash      (color group key)
    ///   +4..+5   material_id     (dye-template variant 1..10;
    ///                            0xFFFF = engine default)
    ///   +6       channel index   (0..N-1; written at inject time)
    ///   +7       r
    ///   +8       g
    ///   +9       b
    ///   +10      0xFF
    ///   +11      repair_byte     (0=pristine, 0x7F=max wear; the
    ///                            engine still accepts 0xFF as a
    ///                            legacy "no override" sentinel and
    ///                            renders it as pristine)
    ///   +13      0x04 for indices 0 and 3 (mirrors natural records)
    struct ChannelState
    {
        std::uint32_t group_hash;
        std::uint8_t  r;
        std::uint8_t  g;
        std::uint8_t  b;
        std::uint16_t material_id;
        std::uint8_t  repair_byte;
    };

    /// Resolve target functions via AOB cascades and install hooks.
    /// Returns true on success. Failure to resolve any required
    /// target disables injection (logged as a warning); the rest of
    /// LT continues to function.
    bool init() noexcept;

    /// Diagnostic: log SlotPopulator observation counters.
    void log_counters() noexcept;

    /// Legacy alias; SlotPopulator hook was retired but the call
    /// site in `prefab_wrapper_swap.cpp` still invokes this on
    /// LT deactivation. Kept as a no-op stat dump.
    void restore_all() noexcept;

    /// Publish per-slot dye state. The detour reads this on the
    /// next DyeCopier invocation. `channels` MUST be exactly
    /// `k_dyeChannelCount` entries; thread-local because the
    /// detour runs on the same thread that publishes.
    void set_slot_dye_state(const ChannelState *channels) noexcept;

    /// Clear the published state. Call after the apply path
    /// completes so subsequent natural equip events go un-modified.
    void clear_slot_dye_state() noexcept;
}
