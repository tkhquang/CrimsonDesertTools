#include "transmog_map.hpp"
#include "shared_state.hpp"

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

    static constexpr const char *k_slotNames[] = {
        "Helm",
        "Chest",
        "Cloak",
        "Gloves",
        "Boots",
    };

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
            return k_slotNames[idx];
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

    // Game slot ID -> TransmogSlot mapping.
    // Discovered via CE entry table dumps + runtime logging.
    std::optional<TransmogSlot> slot_from_game_slot(int16_t gameSlotId)
    {
        switch (gameSlotId)
        {
        case 3:  return TransmogSlot::Helm;
        case 4:  return TransmogSlot::Chest;
        case 5:  return TransmogSlot::Gloves;
        case 6:  return TransmogSlot::Boots;
        case 16: return TransmogSlot::Cloak;
        default: return std::nullopt;
        }
    }

    const char *game_slot_name(int16_t gameSlotId)
    {
        switch (gameSlotId)
        {
        case 0:  return "MainWeapon";
        case 1:  return "SubWeapon";
        case 2:  return "Accessory1";
        case 3:  return "Helm";
        case 4:  return "Chest";
        case 5:  return "Gloves";
        case 6:  return "Boots";
        case 7:  return "Ring1";
        case 8:  return "Ring2";
        case 9:  return "Earring1";
        case 10: return "Earring2";
        case 11: return "Necklace";
        case 12: return "Belt";
        case 13: return "Accessory2";
        case 15: return "Lantern";
        case 16: return "Cloak";
        case 18: return "Earring3";
        case 20: return "Mount";
        default: return "Unknown";
        }
    }

    int16_t game_slot_from_transmog(TransmogSlot slot)
    {
        switch (slot)
        {
        case TransmogSlot::Helm:   return 3;
        case TransmogSlot::Chest:  return 4;
        case TransmogSlot::Gloves: return 5;
        case TransmogSlot::Boots:  return 6;
        case TransmogSlot::Cloak:  return 16;
        default: return -1;
        }
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

} // namespace Transmog
