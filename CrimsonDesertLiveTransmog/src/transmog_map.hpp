#pragma once

#include "shared_state.hpp"

#include <cstdint>
#include <optional>

namespace Transmog
{
    // --- Equip type hashes (Jenkins hashlittle) ---

    inline constexpr uint32_t k_hashHelm = 0x750BE4D5;
    inline constexpr uint32_t k_hashUpperbody = 0x9EFCCE6C;
    inline constexpr uint32_t k_hashHand = 0xD8434271;
    inline constexpr uint32_t k_hashFoot = 0xCCEDA11E;
    inline constexpr uint32_t k_hashLowerbody = 0xE77B2539;
    inline constexpr uint32_t k_hashCloak = 0x4A02EE45;

    std::optional<TransmogSlot> slot_from_equip_hash(uint32_t hash);
    const char *slot_name(TransmogSlot slot);

    bool is_slot_active(TransmogSlot slot);
    /** @brief Look up target item ID for a given equip type hash. Returns 0 if no swap. */
    uint16_t get_target_item_id(uint32_t equipTypeHash);

    /** @brief Map game slot ID (from VisualEquipChange a2) to TransmogSlot. */
    std::optional<TransmogSlot> slot_from_game_slot(int16_t gameSlotId);

    /** @brief Human-readable name for a game slot ID. */
    const char *game_slot_name(int16_t gameSlotId);

    /** @brief TransmogSlot -> game slot ID. Returns -1 if unmapped. */
    int16_t game_slot_from_transmog(TransmogSlot slot);

    /** @brief Look up target item ID by game slot ID. Returns 0 if no swap. */
    uint16_t get_target_item_id_by_slot(int16_t gameSlotId);

    /**
     * @brief True iff two slots share an item picker -- they accept the same items and only differ in which auth-table
     *        slot tag gets the visual.
     *
     * Paired slots in Crimson Desert (engine descriptors share typeCode across both halves of each pair, see
     * ItemNameTable::category_of):
     *   - Earring1 + Earring2  (typeCode 0x08)
     *   - Ring1 + Ring2        (typeCode 0x0A)
     *   - MainHand + OffHand (typeCode 0x00)
     *
     * Returns true for any (a, a) too -- a slot always shares with itself. Used by the item picker so e.g. opening the
     * Ring2 popup shows every ring even though `category_of` returns Ring1 for them.
     */
    bool slots_share_picker(TransmogSlot a, TransmogSlot b);

} // namespace Transmog
