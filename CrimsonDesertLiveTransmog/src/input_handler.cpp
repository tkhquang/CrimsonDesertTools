#include "input_handler.hpp"
#include "dx_overlay.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

namespace Transmog
{
    // --- Hotkey combo storage ---

    static DMK::Config::KeyComboList s_toggleCombos;
    static DMK::Config::KeyComboList s_applyCombos;
    static DMK::Config::KeyComboList s_clearCombos;
    static DMK::Config::KeyComboList s_captureCombos;
    static DMK::Config::KeyComboList s_presetAppendCombos;
    static DMK::Config::KeyComboList s_presetReplaceCombos;
    static DMK::Config::KeyComboList s_presetRemoveCombos;
    static DMK::Config::KeyComboList s_presetNextCombos;
    static DMK::Config::KeyComboList s_presetPrevCombos;
    static DMK::Config::KeyComboList s_overlayToggleCombos;

    // --- Combo setters ---

    void set_toggle_combos(DMK::Config::KeyComboList combos)
    {
        s_toggleCombos = std::move(combos);
    }

    void set_apply_combos(DMK::Config::KeyComboList combos)
    {
        s_applyCombos = std::move(combos);
    }

    void set_clear_combos(DMK::Config::KeyComboList combos)
    {
        s_clearCombos = std::move(combos);
    }

    void set_capture_combos(DMK::Config::KeyComboList combos)
    {
        s_captureCombos = std::move(combos);
    }

    void set_preset_append_combos(DMK::Config::KeyComboList combos)
    {
        s_presetAppendCombos = std::move(combos);
    }

    void set_preset_replace_combos(DMK::Config::KeyComboList combos)
    {
        s_presetReplaceCombos = std::move(combos);
    }

    void set_preset_remove_combos(DMK::Config::KeyComboList combos)
    {
        s_presetRemoveCombos = std::move(combos);
    }

    void set_preset_next_combos(DMK::Config::KeyComboList combos)
    {
        s_presetNextCombos = std::move(combos);
    }

    void set_preset_prev_combos(DMK::Config::KeyComboList combos)
    {
        s_presetPrevCombos = std::move(combos);
    }

    void set_overlay_toggle_combos(DMK::Config::KeyComboList combos)
    {
        s_overlayToggleCombos = std::move(combos);
    }

    // --- Registration ---

    void register_hotkeys()
    {
        auto &inputMgr = DMK::InputManager::get_instance();
        auto &logger = DMK::Logger::get_instance();
        int count = 0;

        if (!s_toggleCombos.empty())
        {
            inputMgr.register_press(
                "ToggleTransmog",
                s_toggleCombos,
                []()
                {
                    // flag_enabled() is the single source of truth used by
                    // every hook and the overlay checkbox. The toggle flips
                    // it so all state stays consistent.
                    auto &ff = flag_enabled();
                    const bool nowEnabled =
                        !ff.load(std::memory_order_relaxed);
                    ff.store(nowEnabled, std::memory_order_relaxed);
                    if (nowEnabled)
                    {
                        DMK::Logger::get_instance().info(
                            "Transmog ON (hotkey) -- applying");
                        Transmog::manual_apply();
                    }
                    else
                    {
                        DMK::Logger::get_instance().info(
                            "Transmog OFF (hotkey) -- restoring original");
                        Transmog::manual_clear();
                    }
                });
            ++count;
        }

        if (!s_applyCombos.empty())
        {
            inputMgr.register_press(
                "ApplyTransmog",
                s_applyCombos,
                []()
                {
                    DMK::Logger::get_instance().info("Apply hotkey pressed");
                    Transmog::manual_apply();
                });
            ++count;
        }

        if (!s_clearCombos.empty())
        {
            inputMgr.register_press(
                "ClearTransmog",
                s_clearCombos,
                []()
                {
                    // Clear also flips flag_enabled to false so a subsequent
                    // Toggle sees cleared == disabled — avoids 2-press bug.
                    DMK::Logger::get_instance().info(
                        "Clear hotkey pressed -- disabling transmog");
                    flag_enabled().store(false, std::memory_order_relaxed);
                    Transmog::manual_clear();
                });
            ++count;
        }

        if (!s_captureCombos.empty())
        {
            inputMgr.register_press(
                "CaptureOutfit",
                s_captureCombos,
                []()
                {
                    DMK::Logger::get_instance().info("Capture hotkey pressed");
                    Transmog::capture_outfit();
                });
            ++count;
        }

        if (!s_presetAppendCombos.empty())
        {
            inputMgr.register_press(
                "PresetAppend",
                s_presetAppendCombos,
                []()
                {
                    // Capture real gear first so append snapshots the actual
                    // loadout, not the previously-active preset's mappings.
                    DMK::Logger::get_instance().info("Preset append hotkey pressed");
                    Transmog::capture_outfit();
                    PresetManager::instance().append_from_state();
                    Transmog::manual_apply();
                });
            ++count;
        }

        if (!s_presetReplaceCombos.empty())
        {
            inputMgr.register_press(
                "PresetReplace",
                s_presetReplaceCombos,
                []()
                {
                    DMK::Logger::get_instance().info("Preset replace hotkey pressed");
                    PresetManager::instance().replace_current_from_state();
                });
            ++count;
        }

        if (!s_presetRemoveCombos.empty())
        {
            inputMgr.register_press(
                "PresetRemove",
                s_presetRemoveCombos,
                []()
                {
                    DMK::Logger::get_instance().info("Preset remove hotkey pressed");
                    PresetManager::instance().remove_current();
                });
            ++count;
        }

        if (!s_presetNextCombos.empty())
        {
            inputMgr.register_press(
                "PresetNext",
                s_presetNextCombos,
                []()
                {
                    auto &pm = PresetManager::instance();
                    pm.next_preset();
                    DMK::Logger::get_instance().info("Preset next hotkey pressed");
                    Transmog::manual_apply();
                });
            ++count;
        }

        if (!s_presetPrevCombos.empty())
        {
            inputMgr.register_press(
                "PresetPrev",
                s_presetPrevCombos,
                []()
                {
                    auto &pm = PresetManager::instance();
                    pm.prev_preset();
                    DMK::Logger::get_instance().info("Preset prev hotkey pressed");
                    Transmog::manual_apply();
                });
            ++count;
        }

        if (!s_overlayToggleCombos.empty())
        {
            inputMgr.register_press(
                "OverlayToggle",
                s_overlayToggleCombos,
                []()
                {
                    toggle_overlay_visible();
                });
            ++count;
        }

        logger.info("Hotkeys registered: {} binding(s)", count);
    }

} // namespace Transmog
