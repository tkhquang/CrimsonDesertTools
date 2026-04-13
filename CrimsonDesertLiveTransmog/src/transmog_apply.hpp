#pragma once

#include "shared_state.hpp"

#include <cstdint>

namespace Transmog
{
    /// Applies transmog by calling SlotPopulator directly with target item.
    /// Builds the 16-byte item data structure and empty swap entry internally.
    void apply_transmog(__int64 a1, std::uint16_t targetId);

    /// Applies transmog using a carrier item to bypass equip gates.
    ///
    /// Temporarily swaps the carrier's descriptor pointer in the global
    /// iteminfo ptrArray to point at the target's descriptor, then calls
    /// SlotPopulator with the carrier's itemId. The game reads the target's
    /// visual data (mesh hashes, CondPrefab, etc.) while all equip gates
    /// see a valid carrier. Restores the original pointer immediately after.
    ///
    /// @param carrierId  Valid Kliff-equippable item for the slot
    /// @param targetId   Item whose visual to display (may be NPC variant)
    void apply_transmog_with_carrier(
        __int64 a1, std::uint16_t carrierId, std::uint16_t targetId);

    /// Resolve the default carrier itemId for a given transmog slot.
    /// Returns 0 if no carrier is needed (item is directly equippable)
    /// or if the carrier name can't be resolved.
    std::uint16_t default_carrier_for_slot(TransmogSlot slot);

    /// Returns true if the given item needs a carrier (has variant meta
    /// or is not player-compatible).
    bool needs_carrier(std::uint16_t itemId);

    /// Single-slot apply: tears down and re-applies only the given slot.
    /// Used by hover-preview to avoid full-gear flicker. Only clears
    /// dispatch cache entries matching this slot's game tag.
    void apply_single_slot_transmog(__int64 a1, std::size_t slotIdx);

    /// Full apply pass: two-phase tear-down + SlotPopulator for all active
    /// slots. Updates dispatch cache, suppress mask, and last-applied state.
    void apply_all_transmog(__int64 a1);

    /// Two-pass clear: tears down orphan fakes (pass A), then re-applies
    /// real equipment from the authoritative entry table (pass B).
    void clear_all_transmog(__int64 a1);

} // namespace Transmog
