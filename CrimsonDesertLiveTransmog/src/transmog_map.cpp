#include "transmog_map.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"

#include <DetourModKit.hpp>

#include <array>
#include <string>
#include <utility>

namespace Transmog
{
    struct EquipHashEntry
    {
        uint32_t hash;
        TransmogSlot slot;
    };

    static constexpr std::array<EquipHashEntry, 6> k_equipHashTable = {{
        {k_hashHelm, TransmogSlot::Helm},
        {k_hashUpperbody, TransmogSlot::Chest},
        {k_hashCloak, TransmogSlot::Cloak},
        {k_hashHand, TransmogSlot::Gloves},
        {k_hashFoot, TransmogSlot::Boots},
        {k_hashLowerbody, TransmogSlot::Chest},
    }};

    // Per-slot display names + engine slot tags + prefab prefixes are
    // sourced from `slot_metadata.hpp::k_slotMetadata`. Each accessor
    // here is a thin lookup against that single table.

    std::optional<TransmogSlot> slot_from_equip_hash(uint32_t hash)
    {
        for (const auto &entry : k_equipHashTable)
        {
            if (entry.hash == hash)
                return entry.slot;
        }
        return std::nullopt;
    }

    const char *slot_name(TransmogSlot slot)
    {
        auto idx = static_cast<std::size_t>(slot);
        if (idx < k_slotCount)
            return k_slotMetadata[idx].displayName;
        return "Unknown";
    }

    bool is_slot_active(TransmogSlot slot)
    {
        auto idx = static_cast<std::size_t>(slot);
        if (idx >= k_slotCount)
            return false;
        return slot_mappings()[idx].active;
    }

    uint16_t get_target_item_id(uint32_t equipTypeHash)
    {
        auto slot = slot_from_equip_hash(equipTypeHash);
        if (!slot.has_value())
            return 0;

        auto idx = static_cast<std::size_t>(*slot);
        auto &mapping = slot_mappings()[idx];
        if (!mapping.active || mapping.targetItemId == 0)
            return 0;

        return mapping.targetItemId;
    }

    // Engine slot tag -> TransmogSlot. Linear search of k_slotMetadata
    // (20 entries; cheap; called sparingly). Returns std::nullopt for
    // tags LT does not manage (e.g. 0x0E or Oongka-only 0x15).
    std::optional<TransmogSlot> slot_from_game_slot(int16_t gameSlotId)
    {
        return slot_from_game_tag(gameSlotId);
    }

    // Friendly engine-tag name. Looks up the matching SlotMetadata by
    // gameTag and returns its displayName; "Unknown" for tags without a
    // managed slot (0x0E, 0x15, anything > 0x15).
    const char *game_slot_name(int16_t gameSlotId)
    {
        if (auto s = slot_from_game_tag(gameSlotId))
            return slot_meta(*s).displayName;
        // Tag 0x15 (OongkaRocket) is intentionally unmanaged but still
        // shows up in [slot-discovery] dumps for diagnostic purposes.
        if (gameSlotId == 0x15)
            return "OongkaRocket";
        return "Unknown";
    }

    int16_t game_slot_from_transmog(TransmogSlot slot)
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= k_slotCount)
            return -1;
        return k_slotMetadata[idx].gameTag;
    }

    uint16_t get_target_item_id_by_slot(int16_t gameSlotId)
    {
        auto slot = slot_from_game_slot(gameSlotId);
        if (!slot.has_value())
            return 0;

        auto idx = static_cast<std::size_t>(*slot);
        auto &mapping = slot_mappings()[idx];
        if (!mapping.active || mapping.targetItemId == 0)
            return 0;

        return mapping.targetItemId;
    }

    bool slots_share_picker(TransmogSlot a, TransmogSlot b)
    {
        if (a == b) return true;

        // Pairs from the engine's typeCode taxonomy. Picker filter
        // checks both directions so order doesn't matter.
        const auto eq = [&](TransmogSlot x, TransmogSlot y) {
            return (a == x && b == y) || (a == y && b == x);
        };
        if (eq(TransmogSlot::Earring1,  TransmogSlot::Earring2))  return true;
        if (eq(TransmogSlot::Ring1,     TransmogSlot::Ring2))     return true;
        if (eq(TransmogSlot::MainHand, TransmogSlot::OffHand))  return true;
        return false;
    }

} // namespace Transmog
