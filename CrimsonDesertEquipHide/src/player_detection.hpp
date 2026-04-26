#pragma once

#include <cstdint>

namespace EquipHide
{
    /** @brief Traverse world system to find protagonist vis_ctrl pointers. */
    void resolve_player_vis_ctrls() noexcept;

    /** @brief Returns true if the given vis_ctrl belongs to a known protagonist. */
    bool is_player_vis_ctrl(uintptr_t a1) noexcept;

    /**
     * @brief Check player-only filter. Returns false (reject) if the actor
     *        is not a known protagonist; admission is unconditional --
     *        equipment hiding only applies to the playable cast (Kliff,
     *        Damiane, Oongka), never to NPCs.
     *
     * Side effects: triggers resolve cycle (global chain) or caches new
     * vis ctrls (fallback mode).
     */
    bool check_player_filter(uintptr_t a1) noexcept;

} // namespace EquipHide
