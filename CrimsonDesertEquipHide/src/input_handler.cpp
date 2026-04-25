#include "input_handler.hpp"
#include "armor_injection.hpp"
#include "categories.hpp"
#include "equip_hide.hpp"
#include "player_detection.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace EquipHide
{
    namespace
    {
        // Process-lifetime stash for the per-binding cancellation
        // guards. Each guard owns a shared_ptr<atomic<bool>> that gates
        // the user callback; dropping the guard cancels the binding.
        std::vector<DMK::Config::InputBindingGuard> &binding_guards()
        {
            static std::vector<DMK::Config::InputBindingGuard> s_guards;
            return s_guards;
        }
    } // namespace

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
        /* Publish the request before attempting the write inline. If
           apply_direct_vis_write loses the lock race against the
           mid-hook or resolve poll, the next mid-hook tick clears the
           flag and re-runs the write -- the toggle is no longer
           silently dropped on contention. The inline call below still
           executes on the common (uncontended) path so single-shot
           hotkey latency is unchanged. */
        needs_direct_write().store(true, std::memory_order_release);
        apply_direct_vis_write();
    }

    void register_hotkeys()
    {
        auto &logger = DMK::Logger::get_instance();
        auto &guards = binding_guards();
        // 2 globals + 3 bindings per category.
        const std::size_t expected = 2 + 3 * CATEGORY_COUNT;
        guards.reserve(expected);

        auto add_binding = [&](std::string_view section,
                               std::string_view ini_key,
                               std::string log_name,
                               std::string input_name,
                               std::function<void()> on_press,
                               std::string_view default_value)
        {
            // Empty / "NONE" INI values are recognised as opt-out sentinels
            // by DMK::Config::parse_key_combo_list and produce an unbound
            // binding silently. The binding name remains addressable so a
            // later non-empty update_binding_combos can attach a real combo
            // on a live INI reload.
            guards.push_back(DMK::Config::register_press_combo(
                section, ini_key, log_name,
                input_name, std::move(on_press), default_value));
        };

        add_binding(
            "General", "ShowAllHotkey", "Show All Hotkey",
            "ShowAll",
            []()
            {
                auto &st = category_states();
                for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                    st[i].hidden.store(false, std::memory_order_relaxed);

                DMK::Logger::get_instance().info(
                    "Equip hide: all categories VISIBLE");
                flush_visibility();
            },
            "");

        add_binding(
            "General", "HideAllHotkey", "Hide All Hotkey",
            "HideAll",
            []()
            {
                auto &st = category_states();
                for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                    st[i].hidden.store(true, std::memory_order_relaxed);

                DMK::Logger::get_instance().info(
                    "Equip hide: all categories HIDDEN");
                flush_visibility();
            },
            "");

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto cat = static_cast<Category>(i);
            const std::string section{category_section(cat)};

            // Default toggle binding: shields/helm/mask are active by
            // default with the literal "V" combo that produces an empty
            // press_combo (no key bound, INI value editable to taste).
            const bool active = (cat == Category::Shields ||
                                 cat == Category::Helm ||
                                 cat == Category::Mask);
            const char *defaultToggle = active ? "V" : "";

            const std::string toggleName = "ToggleEquip_" + section;
            const std::string showName = "ShowEquip_" + section;
            const std::string hideName = "HideEquip_" + section;

            add_binding(
                section, "ToggleHotkey", section + " Toggle Hotkey",
                toggleName,
                [i, section]()
                {
                    auto &st = category_states();
                    auto &log = DMK::Logger::get_instance();

                    // IndependentToggle has lost its prior cross-binding
                    // semantics (DMK::Config::register_press_combo treats
                    // each binding independently). The flag now reads as
                    // "always independent": every binding flips only its
                    // own slot. Users wanting a synchronised toggle should
                    // bind the same combo to ShowAll/HideAll.
                    const bool newHidden =
                        !st[i].hidden.load(std::memory_order_relaxed);
                    st[i].hidden.store(newHidden, std::memory_order_relaxed);

                    log.info("Equip hide [{}]: {}", section,
                             newHidden ? "HIDDEN" : "VISIBLE");
                    flush_visibility();
                },
                defaultToggle);

            add_binding(
                section, "ShowHotkey", section + " Show Hotkey",
                showName,
                [i, section]()
                {
                    category_states()[i].hidden.store(
                        false, std::memory_order_relaxed);
                    DMK::Logger::get_instance().info(
                        "Equip hide [{}]: VISIBLE", section);
                    flush_visibility();
                },
                "");

            add_binding(
                section, "HideHotkey", section + " Hide Hotkey",
                hideName,
                [i, section]()
                {
                    category_states()[i].hidden.store(
                        true, std::memory_order_relaxed);
                    DMK::Logger::get_instance().info(
                        "Equip hide [{}]: HIDDEN", section);
                    flush_visibility();
                },
                "");
        }

        logger.info("Hotkeys registered: {} binding(s)", guards.size());
    }

    void clear_hotkey_guards() noexcept
    {
        binding_guards().clear();
    }

} // namespace EquipHide
