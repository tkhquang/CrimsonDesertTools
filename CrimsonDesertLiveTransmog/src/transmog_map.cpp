#include "transmog_map.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"

#include <array>
#include <cstddef>

namespace Transmog
{
    namespace
    {
        struct EquipHashEntry
        {
            std::uint32_t hash;
            TransmogSlot slot;
        };

        constexpr std::array<EquipHashEntry, 6> k_equipHashTable = {{
            {k_hashHelm, TransmogSlot::Helm},
            {k_hashUpperbody, TransmogSlot::Chest},
            {k_hashCloak, TransmogSlot::Cloak},
            {k_hashHand, TransmogSlot::Gloves},
            {k_hashFoot, TransmogSlot::Boots},
            {k_hashLowerbody, TransmogSlot::Chest},
        }};
    } // namespace

    // Per-slot display names + engine slot tags + prefab prefixes are sourced from `slot_metadata.hpp::k_slotMetadata`.
    // Each accessor here is a thin lookup against that single table.

    std::optional<TransmogSlot> slot_from_equip_hash(std::uint32_t hash) noexcept
    {
        for (const auto &entry : k_equipHashTable)
        {
            if (entry.hash == hash)
                return entry.slot;
        }
        return std::nullopt;
    }

    const char *slot_name(TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx < k_slotCount)
            return k_slotMetadata[idx].displayName;
        return "Unknown";
    }

    bool is_slot_active(TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= k_slotCount)
            return false;
        return slot_mappings()[idx].active;
    }

    std::uint16_t get_target_item_id(std::uint32_t equipTypeHash) noexcept
    {
        const auto slot = slot_from_equip_hash(equipTypeHash);
        if (!slot.has_value())
            return 0;

        const auto idx = static_cast<std::size_t>(*slot);
        const auto &mapping = slot_mappings()[idx];
        if (!mapping.active || mapping.targetItemId == 0)
            return 0;

        return mapping.targetItemId;
    }

    // Engine slot tag -> TransmogSlot. Linear search of k_slotMetadata. The search is cheap and runs sparingly.
    // Returns std::nullopt for tags LT does not manage (e.g. the Oongka-only 0x15).
    std::optional<TransmogSlot> slot_from_game_slot(std::int16_t gameSlotId) noexcept
    {
        return slot_from_game_tag(gameSlotId);
    }

    // Friendly engine-tag name. Looks up the matching SlotMetadata by gameTag and returns its displayName. Tags
    // without a managed slot (0x15, anything the metadata table does not carry) read "Unknown".
    const char *game_slot_name(std::int16_t gameSlotId) noexcept
    {
        if (auto s = slot_from_game_tag(gameSlotId))
            return slot_meta(*s).displayName;
        // Tag 0x15 (OongkaRocket) is intentionally unmanaged but still shows up in [slot-discovery] dumps for
        // diagnostic purposes.
        if (gameSlotId == 0x15)
            return "OongkaRocket";
        return "Unknown";
    }

    std::int16_t game_slot_from_transmog(TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= k_slotCount)
            return -1;
        return k_slotMetadata[idx].gameTag;
    }

    std::uint16_t get_target_item_id_by_slot(std::int16_t gameSlotId) noexcept
    {
        const auto slot = slot_from_game_slot(gameSlotId);
        if (!slot.has_value())
            return 0;

        const auto idx = static_cast<std::size_t>(*slot);
        const auto &mapping = slot_mappings()[idx];
        if (!mapping.active || mapping.targetItemId == 0)
            return 0;

        return mapping.targetItemId;
    }

    bool slots_share_picker(TransmogSlot a, TransmogSlot b) noexcept
    {
        if (a == b)
            return true;

        // Pairs from the engine's typeCode taxonomy. The picker filter checks both directions, so order does not
        // matter.
        const auto eq = [&](TransmogSlot x, TransmogSlot y) { return (a == x && b == y) || (a == y && b == x); };
        if (eq(TransmogSlot::Earring1, TransmogSlot::Earring2))
            return true;
        if (eq(TransmogSlot::Ring1, TransmogSlot::Ring2))
            return true;
        if (eq(TransmogSlot::MainHand, TransmogSlot::OffHand))
            return true;
        return false;
    }

} // namespace Transmog
