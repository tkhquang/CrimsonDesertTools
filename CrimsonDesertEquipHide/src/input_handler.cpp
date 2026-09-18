#include "input_handler.hpp"
#include "armor_injection.hpp"
#include "categories.hpp"
#include "equip_hide.hpp"
#include "player_detection.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <DetourModKit/config.hpp>
#include <DetourModKit/input.hpp>
#include <DetourModKit/logger.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace EquipHide
{
    void flush_visibility() noexcept
    {
        arm_flush_guard();
        update_hidden_mask();

        if (!flag_fallback_mode().load(std::memory_order_relaxed))
            resolve_player_vis_ctrls();

        // Reset injection flags so newly-toggled categories get entries created.
        auto &ps = player_state();
        for (int i = 0; i < k_maxProtagonists; ++i)
            ps.armorInjected[i].store(false, std::memory_order_relaxed);

        inject_armor_entries();
        /* Publish the request before attempting the write inline. If apply_direct_vis_write loses the lock race against
           the mid-hook or resolve poll, the next mid-hook tick clears the flag and re-runs the write -- the toggle is
           no longer silently dropped on contention. The inline call below still executes on the common (uncontended)
           path so single-shot hotkey latency is unchanged. */
        needs_direct_write().store(true, std::memory_order_release);
        apply_direct_vis_write();
    }

    void register_hotkeys(DMK::input::Scope &scope)
    {
        auto &logger = DMK::log();
        // 2 globals + 3 bindings per category.
        const auto before = scope.size();

        // Section-scoped binder, so the INI section name is written once instead of heading every call. The
        // per-category loop below builds its own binder per iteration for the same reason.
        const DMK::config::SectionBinder general = DMK::config::section("General");

        // Every binding passes false as press_combo's trailing `consume`, which registers a "<ini_key>.Consume"
        // bool defaulting OFF. Registering it costs nothing at runtime -- the input engine only installs its XInput
        // interception once some binding is actually set true -- and withholding it would hide the option entirely,
        // because an INI key that was never registered is ignored without a warning. Whether a binding can USE
        // suppression is not fixed here either: the combo is user-editable, so any binding can become a gamepad
        // binding. Suppression is honored for digital gamepad buttons and the mouse wheel only, masks just the
        // TRIGGER (never the modifier), and never affects a keyboard combo.
        //
        // An empty default and the literal "NONE" are opt-out sentinels: press_combo registers an unbound but
        // addressable binding silently, so a later non-empty INI value attaches a real combo on a live reload
        // without re-registering.

        scope.add(general.press_combo(
            "ShowAllHotkey",
            "Show All Hotkey",
            "ShowAll",
            []()
            {
                auto &st = category_states();
                for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                    st[i].hidden.store(false, std::memory_order_relaxed);

                DMK::log().info("Equip hide: all categories VISIBLE");
                flush_visibility();
            },
            "",
            false
        ));

        scope.add(general.press_combo(
            "HideAllHotkey",
            "Hide All Hotkey",
            "HideAll",
            []()
            {
                auto &st = category_states();
                for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                    st[i].hidden.store(true, std::memory_order_relaxed);

                DMK::log().info("Equip hide: all categories HIDDEN");
                flush_visibility();
            },
            "",
            false
        ));

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto cat = static_cast<Category>(i);
            const std::string section{category_section(cat)};
            const DMK::config::SectionBinder category = DMK::config::section(section);

            // Default toggle binding: shields/helm/mask are active by default with the literal "V" combo that produces
            // an empty press_combo (no key bound, INI value editable to taste).
            const bool active = (cat == Category::Shields || cat == Category::Helm || cat == Category::Mask);
            const char *defaultToggle = active ? "V" : "";

            const std::string toggleName = "ToggleEquip_" + section;
            const std::string showName = "ShowEquip_" + section;
            const std::string hideName = "HideEquip_" + section;

            scope.add(category.press_combo(
                "ToggleHotkey",
                section + " Toggle Hotkey",
                toggleName,
                [i, section]()
                {
                    auto &st = category_states();
                    auto &log = DMK::log();

                    // The IndependentToggle flag is effectively always on: each binding flips only its own slot,
                    // because DMK::config::press_combo treats every binding independently. For a synchronised
                    // toggle, bind the same combo to ShowAll/HideAll.
                    const bool newHidden = !st[i].hidden.load(std::memory_order_relaxed);
                    st[i].hidden.store(newHidden, std::memory_order_relaxed);

                    log.info("Equip hide [{}]: {}", section, newHidden ? "HIDDEN" : "VISIBLE");
                    flush_visibility();
                },
                defaultToggle,
                false
            ));

            scope.add(category.press_combo(
                "ShowHotkey",
                section + " Show Hotkey",
                showName,
                [i, section]()
                {
                    category_states()[i].hidden.store(false, std::memory_order_relaxed);
                    DMK::log().info("Equip hide [{}]: VISIBLE", section);
                    flush_visibility();
                },
                "",
                false
            ));

            scope.add(category.press_combo(
                "HideHotkey",
                section + " Hide Hotkey",
                hideName,
                [i, section]()
                {
                    category_states()[i].hidden.store(true, std::memory_order_relaxed);
                    DMK::log().info("Equip hide [{}]: HIDDEN", section);
                    flush_visibility();
                },
                "",
                false
            ));
        }

        logger.info("Hotkeys registered: {} binding(s)", scope.size() - before);
    }

} // namespace EquipHide
