#include "input_handler.hpp"
#include "dx_overlay.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_map.hpp"

#include <DetourModKit/config.hpp>
#include <DetourModKit/input.hpp>
#include <DetourModKit/logger.hpp>

#include <functional>
#include <optional>
#include <string_view>
#include <utility>

namespace Transmog
{
    void register_hotkeys(DMK::input::Scope &scope)
    {
        auto &logger = DMK::log();
        const auto before = scope.size();

        // Section-scoped binders, so each INI section name is written once here instead of heading every call.
        const DMK::config::SectionBinder general = DMK::config::section("General");
        const DMK::config::SectionBinder presets = DMK::config::section("Presets");

        // Every binding passes false as press_combo's trailing `consume`, which registers a "<ini_key>.Consume"
        // bool defaulting OFF. Registering it costs nothing at runtime - the input engine only installs its XInput
        // interception once some binding is actually set true - and withholding it would hide the option entirely,
        // because an INI key that was never registered is ignored without a warning. Whether a binding can USE
        // suppression is not fixed here either: the combo is user-editable, so any binding can become a gamepad
        // binding. Suppression is honored for digital gamepad buttons and the mouse wheel only, masks just the
        // TRIGGER (never the modifier), and never affects a keyboard combo.
        //
        // An empty default and the literal "NONE" are opt-out sentinels: press_combo registers an unbound but
        // addressable binding silently, so a later non-empty INI value attaches a real combo on a live reload
        // without re-registering.

        scope.add(general.press_combo(
            "ToggleHotkey",
            "Toggle Hotkey",
            "ToggleTransmog",
            []() -> void
            {
                // flag_enabled() is the single source of truth used by every hook and the overlay checkbox. The
                // toggle flips it so all state stays consistent.
                auto &ff = flag_enabled();
                const bool now_enabled = !ff.load(std::memory_order_relaxed);
                ff.store(now_enabled, std::memory_order_relaxed);
                if (now_enabled)
                {
                    DMK::log().info("Transmog ON (hotkey) - applying");
                    Transmog::manual_apply();
                }
                else
                {
                    DMK::log().info("Transmog OFF (hotkey) - restoring original");
                    Transmog::manual_clear();
                }
            },
            "",
            false
        ));

        scope.add(general.press_combo(
            "ApplyHotkey",
            "Apply Transmog Hotkey",
            "ApplyTransmog",
            []() -> void
            {
                DMK::log().info("Apply hotkey pressed");
                Transmog::manual_apply();
            },
            "",
            false
        ));

        scope.add(general.press_combo(
            "ClearHotkey",
            "Clear Transmog Hotkey",
            "ClearTransmog",
            []() -> void
            {
                // Clear also stores false into flag_enabled, so the next Toggle reads cleared as disabled.
                DMK::log().info("Clear hotkey pressed - disabling transmog");
                flag_enabled().store(false, std::memory_order_relaxed);
                Transmog::manual_clear();
            },
            "",
            false
        ));

        scope.add(general.press_combo(
            "CaptureHotkey",
            "Capture Outfit Hotkey",
            "CaptureOutfit",
            []() -> void
            {
                DMK::log().info("Capture hotkey pressed");
                Transmog::capture_outfit();
            },
            "",
            false
        ));

        scope.add(presets.press_combo(
            "AppendHotkey",
            "Append Preset Hotkey",
            "PresetAppend",
            []() -> void
            {
                DMK::log().info("Preset append hotkey pressed");
                PresetManager::instance().append_from_state();
                Transmog::manual_apply();
            },
            "",
            false
        ));

        scope.add(presets.press_combo(
            "ReplaceHotkey",
            "Replace Preset Hotkey",
            "PresetReplace",
            []() -> void
            {
                DMK::log().info("Preset replace hotkey pressed");
                PresetManager::instance().replace_current_from_state();
            },
            "",
            false
        ));

        scope.add(presets.press_combo(
            "RemoveHotkey",
            "Remove Preset Hotkey",
            "PresetRemove",
            []() -> void
            {
                DMK::log().info("Preset remove hotkey pressed");
                PresetManager::instance().remove_current();
            },
            "",
            false
        ));

        scope.add(presets.press_combo(
            "NextHotkey",
            "Next Preset Hotkey",
            "PresetNext",
            []() -> void
            {
                auto &pm = PresetManager::instance();
                pm.next_preset();
                DMK::log().info("Preset next hotkey pressed");
                Transmog::manual_apply();
            },
            "",
            false
        ));

        scope.add(presets.press_combo(
            "PrevHotkey",
            "Previous Preset Hotkey",
            "PresetPrev",
            []() -> void
            {
                auto &pm = PresetManager::instance();
                pm.prev_preset();
                DMK::log().info("Preset prev hotkey pressed");
                Transmog::manual_apply();
            },
            "",
            false
        ));

        scope.add(general.press_combo(
            "OverlayToggleHotkey",
            "Overlay Toggle Hotkey",
            "OverlayToggle",
            []() { toggle_overlay_visible(); },
            "Home",
            false
        ));

        logger.info("Hotkeys registered: {} binding(s)", scope.size() - before);
    }

} // namespace Transmog
