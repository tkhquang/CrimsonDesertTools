#ifndef TRANSMOG_INPUT_HANDLER_HPP
#define TRANSMOG_INPUT_HANDLER_HPP

#include <DetourModKit.hpp>

namespace Transmog
{
    /** @brief Register all configured hotkey bindings with the input manager. */
    void register_hotkeys();

    /** @brief Set the key combo(s) that toggle transmog on/off. */
    void set_toggle_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that force-apply the current preset. */
    void set_apply_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that clear all transmog and restore originals. */
    void set_clear_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that capture the currently equipped gear. */
    void set_capture_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that append a new preset from current state. */
    void set_preset_append_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that replace the active preset. */
    void set_preset_replace_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that remove the active preset. */
    void set_preset_remove_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that cycle to the next preset. */
    void set_preset_next_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that cycle to the previous preset. */
    void set_preset_prev_combos(DMK::Config::KeyComboList combos);

    /** @brief Set the key combo(s) that toggle overlay visibility. */
    void set_overlay_toggle_combos(DMK::Config::KeyComboList combos);

} // namespace Transmog

#endif // TRANSMOG_INPUT_HANDLER_HPP
