#include "input_handler.hpp"
#include "dx_overlay.hpp"
#include "preset_manager.hpp"
#include "shared_state.hpp"
#include "transmog.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Transmog
{
    namespace
    {
        // Process-lifetime stash for the per-binding cancellation
        // guards returned by register_press_combo. Each guard owns a
        // shared_ptr<atomic<bool>> that gates the user callback;
        // dropping the guard cancels the binding. We only need the
        // guards for shutdown ordering (they release in destructor
        // order), so a plain vector with reserve is fine.
        std::vector<DMK::Config::InputBindingGuard> &binding_guards()
        {
            static std::vector<DMK::Config::InputBindingGuard> s_guards;
            return s_guards;
        }

        DMK::Config::InputBindingGuard add_binding(
            std::string_view section,
            std::string_view ini_key,
            std::string_view log_name,
            std::string_view input_name,
            std::function<void()> on_press,
            std::string_view default_value)
        {
            // Empty / "NONE" INI values are recognised as opt-out sentinels
            // by DMK::Config::parse_key_combo_list and produce an unbound
            // binding silently. The binding name remains addressable for a
            // later non-empty update_binding_combos when the user assigns a
            // real combo on a live INI reload, with no consumer-side
            // wrapper required.
            return DMK::Config::register_press_combo(
                section, ini_key, log_name,
                input_name, std::move(on_press), default_value);
        }
    } // namespace

    void register_hotkeys()
    {
        auto &logger = DMK::Logger::get_instance();
        auto &guards = binding_guards();
        constexpr std::size_t k_expected = 10;
        guards.reserve(k_expected);

        guards.push_back(add_binding(
            "General", "ToggleHotkey", "Toggle Hotkey", "ToggleTransmog",
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
            },
            ""));

        guards.push_back(add_binding(
            "General", "ApplyHotkey", "Apply Transmog Hotkey", "ApplyTransmog",
            []()
            {
                DMK::Logger::get_instance().info("Apply hotkey pressed");
                Transmog::manual_apply();
            },
            ""));

        guards.push_back(add_binding(
            "General", "ClearHotkey", "Clear Transmog Hotkey", "ClearTransmog",
            []()
            {
                // Clear also flips flag_enabled to false so a subsequent
                // Toggle sees cleared == disabled (avoids 2-press bug).
                DMK::Logger::get_instance().info(
                    "Clear hotkey pressed -- disabling transmog");
                flag_enabled().store(false, std::memory_order_relaxed);
                Transmog::manual_clear();
            },
            ""));

        guards.push_back(add_binding(
            "General", "CaptureHotkey", "Capture Outfit Hotkey", "CaptureOutfit",
            []()
            {
                DMK::Logger::get_instance().info("Capture hotkey pressed");
                Transmog::capture_outfit();
            },
            ""));

        guards.push_back(add_binding(
            "Presets", "AppendHotkey", "Append Preset Hotkey", "PresetAppend",
            []()
            {
                DMK::Logger::get_instance().info("Preset append hotkey pressed");
                PresetManager::instance().append_from_state();
                Transmog::manual_apply();
            },
            ""));

        guards.push_back(add_binding(
            "Presets", "ReplaceHotkey", "Replace Preset Hotkey", "PresetReplace",
            []()
            {
                DMK::Logger::get_instance().info("Preset replace hotkey pressed");
                PresetManager::instance().replace_current_from_state();
            },
            ""));

        guards.push_back(add_binding(
            "Presets", "RemoveHotkey", "Remove Preset Hotkey", "PresetRemove",
            []()
            {
                DMK::Logger::get_instance().info("Preset remove hotkey pressed");
                PresetManager::instance().remove_current();
            },
            ""));

        guards.push_back(add_binding(
            "Presets", "NextHotkey", "Next Preset Hotkey", "PresetNext",
            []()
            {
                auto &pm = PresetManager::instance();
                pm.next_preset();
                DMK::Logger::get_instance().info("Preset next hotkey pressed");
                Transmog::manual_apply();
            },
            ""));

        guards.push_back(add_binding(
            "Presets", "PrevHotkey", "Previous Preset Hotkey", "PresetPrev",
            []()
            {
                auto &pm = PresetManager::instance();
                pm.prev_preset();
                DMK::Logger::get_instance().info("Preset prev hotkey pressed");
                Transmog::manual_apply();
            },
            ""));

        guards.push_back(add_binding(
            "General", "OverlayToggleHotkey", "Overlay Toggle Hotkey", "OverlayToggle",
            []()
            {
                toggle_overlay_visible();
            },
            "Home"));

        logger.info("Hotkeys registered: {} binding(s)", guards.size());
    }

    void clear_hotkey_guards() noexcept
    {
        binding_guards().clear();
    }

} // namespace Transmog
