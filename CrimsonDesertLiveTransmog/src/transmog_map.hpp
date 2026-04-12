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

    /** @brief TransmogSlot → game slot ID. Returns -1 if unmapped. */
    int16_t game_slot_from_transmog(TransmogSlot slot);

    /** @brief Look up target item ID by game slot ID. Returns 0 if no swap. */
    uint16_t get_target_item_id_by_slot(int16_t gameSlotId);

} // namespace Transmog
