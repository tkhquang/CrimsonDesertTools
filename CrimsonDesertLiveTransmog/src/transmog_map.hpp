#ifndef TRANSMOG_TRANSMOG_MAP_HPP
#define TRANSMOG_TRANSMOG_MAP_HPP

#include "shared_state.hpp"

#include <cstdint>
#include <optional>

namespace Transmog
{
    // Equip type hashes (Jenkins hashlittle)

    inline constexpr std::uint32_t HASH_HELM = 0x750BE4D5;
    inline constexpr std::uint32_t HASH_UPPERBODY = 0x9EFCCE6C;
    inline constexpr std::uint32_t HASH_HAND = 0xD8434271;
    inline constexpr std::uint32_t HASH_FOOT = 0xCCEDA11E;
    inline constexpr std::uint32_t HASH_LOWERBODY = 0xE77B2539;
    inline constexpr std::uint32_t HASH_CLOAK = 0x4A02EE45;

    /** @brief Map an equip type hash to a TransmogSlot. Returns std::nullopt for an unmapped hash. */
    [[nodiscard]] std::optional<TransmogSlot> slot_from_equip_hash(std::uint32_t hash) noexcept;

    /** @brief Human-readable name for a TransmogSlot. Returns "Unknown" for an out-of-range slot. */
    [[nodiscard]] const char *slot_name(TransmogSlot slot) noexcept;

    /** @brief True when the slot carries a live transmog mapping. */
    [[nodiscard]] bool is_slot_active(TransmogSlot slot) noexcept;

    /** @brief Look up target item ID for a given equip type hash. Returns 0 if no swap. */
    [[nodiscard]] std::uint16_t get_target_item_id(std::uint32_t equip_type_hash) noexcept;

    /** @brief Map game slot ID (from VisualEquipChange a2) to TransmogSlot. */
    [[nodiscard]] std::optional<TransmogSlot> slot_from_game_slot(std::int16_t game_slot_id) noexcept;

    /** @brief Human-readable name for a game slot ID. */
    [[nodiscard]] const char *game_slot_name(std::int16_t game_slot_id) noexcept;

    /** @brief TransmogSlot -> game slot ID. Returns -1 if unmapped. */
    [[nodiscard]] std::int16_t game_slot_from_transmog(TransmogSlot slot) noexcept;

    /** @brief Look up target item ID by game slot ID. Returns 0 if no swap. */
    [[nodiscard]] std::uint16_t get_target_item_id_by_slot(std::int16_t game_slot_id) noexcept;

    /**
     * @brief True iff two slots share an item picker. Such slots accept the same items and only differ in which
     *        auth-table slot tag gets the visual.
     *
     * Paired slots in Crimson Desert (engine descriptors share type_code across both halves of each pair, see
     * ItemNameTable::category_of):
     *   - Earring1 + Earring2  (type_code 0x08)
     *   - Ring1 + Ring2        (type_code 0x0A)
     *   - MainHand + OffHand (type_code 0x00)
     *
     * Returns true for any (a, a) too. A slot always shares with itself. The item picker uses this so that opening
     * the Ring2 popup shows every ring even though `category_of` returns Ring1 for them.
     */
    [[nodiscard]] bool slots_share_picker(TransmogSlot a, TransmogSlot b) noexcept;

} // namespace Transmog

#endif // TRANSMOG_TRANSMOG_MAP_HPP
