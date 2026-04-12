#pragma once

#include <DetourModKit.hpp>

namespace Transmog
{
    void register_hotkeys();
    void set_toggle_combos(DMK::Config::KeyComboList combos);
    void set_apply_combos(DMK::Config::KeyComboList combos);
    void set_clear_combos(DMK::Config::KeyComboList combos);
    void set_capture_combos(DMK::Config::KeyComboList combos);
    void set_preset_append_combos(DMK::Config::KeyComboList combos);
    void set_preset_replace_combos(DMK::Config::KeyComboList combos);
    void set_preset_remove_combos(DMK::Config::KeyComboList combos);
    void set_preset_next_combos(DMK::Config::KeyComboList combos);
    void set_preset_prev_combos(DMK::Config::KeyComboList combos);

} // namespace Transmog
