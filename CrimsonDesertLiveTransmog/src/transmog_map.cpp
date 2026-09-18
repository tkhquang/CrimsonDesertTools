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

        constexpr std::array<EquipHashEntry, 6> EQUIP_HASH_TABLE = {{
            {HASH_HELM, TransmogSlot::Helm},
            {HASH_UPPERBODY, TransmogSlot::Chest},
            {HASH_CLOAK, TransmogSlot::Cloak},
            {HASH_HAND, TransmogSlot::Gloves},
            {HASH_FOOT, TransmogSlot::Boots},
            {HASH_LOWERBODY, TransmogSlot::Chest},
        }};
    } // namespace

    // Per-slot display names + engine slot tags + prefab prefixes are sourced from `slot_metadata.hpp::SLOT_METADATA`.
    // Each accessor here is a thin lookup against that single table.

    std::optional<TransmogSlot> slot_from_equip_hash(std::uint32_t hash) noexcept
    {
        for (const auto &entry : EQUIP_HASH_TABLE)
        {
            if (entry.hash == hash)
                return entry.slot;
        }
        return std::nullopt;
    }

    const char *slot_name(TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx < SLOT_COUNT)
            return SLOT_METADATA[idx].display_name;
        return "Unknown";
    }

    bool is_slot_active(TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= SLOT_COUNT)
            return false;
        return slot_mappings()[idx].active;
    }

    std::uint16_t get_target_item_id(std::uint32_t equip_type_hash) noexcept
    {
        const auto slot = slot_from_equip_hash(equip_type_hash);
        if (!slot.has_value())
            return 0;

        const auto idx = static_cast<std::size_t>(*slot);
        const auto &mapping = slot_mappings()[idx];
        if (!mapping.active || mapping.target_item_id == 0)
            return 0;

        return mapping.target_item_id;
    }

    // Engine slot tag -> TransmogSlot. Linear search of SLOT_METADATA. The search is cheap and runs sparingly.
    // Returns std::nullopt for tags LT does not manage (e.g. the Oongka-only 0x15).
    std::optional<TransmogSlot> slot_from_game_slot(std::int16_t game_slot_id) noexcept
    {
        return slot_from_game_tag(game_slot_id);
    }

    // Friendly engine-tag name. Looks up the matching SlotMetadata by game_tag and returns its display_name. Tags
    // without a managed slot (0x15, anything the metadata table does not carry) read "Unknown".
    const char *game_slot_name(std::int16_t game_slot_id) noexcept
    {
        if (auto s = slot_from_game_tag(game_slot_id))
            return slot_meta(*s).display_name;
        // Tag 0x15 (OongkaRocket) is intentionally unmanaged but still shows up in [slot-discovery] dumps for
        // diagnostic purposes.
        if (game_slot_id == 0x15)
            return "OongkaRocket";
        return "Unknown";
    }

    std::int16_t game_slot_from_transmog(TransmogSlot slot) noexcept
    {
        const auto idx = static_cast<std::size_t>(slot);
        if (idx >= SLOT_COUNT)
            return -1;
        return SLOT_METADATA[idx].game_tag;
    }

    std::uint16_t get_target_item_id_by_slot(std::int16_t game_slot_id) noexcept
    {
        const auto slot = slot_from_game_slot(game_slot_id);
        if (!slot.has_value())
            return 0;

        const auto idx = static_cast<std::size_t>(*slot);
        const auto &mapping = slot_mappings()[idx];
        if (!mapping.active || mapping.target_item_id == 0)
            return 0;

        return mapping.target_item_id;
    }

    bool slots_share_picker(TransmogSlot a, TransmogSlot b) noexcept
    {
        if (a == b)
            return true;

        // Pairs from the engine's type_code taxonomy. The picker filter checks both directions, so order does not
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
