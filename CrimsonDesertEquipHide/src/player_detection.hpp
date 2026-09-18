#ifndef EQUIPHIDE_PLAYER_DETECTION_HPP
#define EQUIPHIDE_PLAYER_DETECTION_HPP

#include <cstdint>

namespace EquipHide
{
    /** @brief Traverse world system to find protagonist vis_ctrl pointers. */
    void resolve_player_vis_ctrls() noexcept;

    /** @brief Returns true if the given vis_ctrl belongs to a known protagonist. */
    [[nodiscard]] bool is_player_vis_ctrl(uintptr_t a1) noexcept;

    /**
     * @brief Check player-only filter. Returns false (reject) if the actor is not a known protagonist.
     * @details Admission is unconditional. The mod hides equipment only for the playable cast (Kliff, Damiane,
     *          Oongka), never for an NPC.
     * @param a1 The visibility-control pointer the engine passed to the decision function.
     * @return true when the actor belongs to the tracked protagonist set.
     * @note Side effects: it triggers a resolve cycle (global chain) or caches a new vis ctrl (fallback mode).
     */
    [[nodiscard]] bool check_player_filter(uintptr_t a1) noexcept;

} // namespace EquipHide

#endif // EQUIPHIDE_PLAYER_DETECTION_HPP
