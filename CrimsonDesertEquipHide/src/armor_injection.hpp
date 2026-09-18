#ifndef EQUIPHIDE_ARMOR_INJECTION_HPP
#define EQUIPHIDE_ARMOR_INJECTION_HPP

namespace EquipHide
{
    /**
     * @brief Inject PartInOutSocket map entries for hidden armor parts.
     * @details Armor parts have no PartInOutSocket entries in vanilla. To hide them, inject new entries with Visible=2
     *          via the game's map insertion function.
     * @note Best-effort: a faulting or stale part map skips that vis controller and the pass continues.
     */
    void inject_armor_entries() noexcept;

} // namespace EquipHide

#endif // EQUIPHIDE_ARMOR_INJECTION_HPP
