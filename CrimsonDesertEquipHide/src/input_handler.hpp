#pragma once

#include <DetourModKit.hpp>

namespace EquipHide
{
    /** @brief Register all category and global hotkeys with the input manager. */
    void register_hotkeys();

    /** @brief Flush visibility state: re-resolve players, re-inject, re-write vis bytes. */
    void flush_visibility() noexcept;

    /** @brief Set the global show-all hotkey combos (called from config). */
    void set_show_all_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the global hide-all hotkey combos (called from config). */
    void set_hide_all_combos(DMK::Config::KeyComboList combos);

} // namespace EquipHide
