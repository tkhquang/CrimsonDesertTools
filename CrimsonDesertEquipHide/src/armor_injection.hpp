#pragma once

namespace EquipHide
{
    /**
     * @brief Inject PartInOutSocket map entries for hidden armor parts.
     * @details Armor parts have no PartInOutSocket entries in vanilla. To hide them,
     *          inject new entries with Visible=2 via the game's map insertion function.
     */
    void inject_armor_entries() noexcept;

} // namespace EquipHide
