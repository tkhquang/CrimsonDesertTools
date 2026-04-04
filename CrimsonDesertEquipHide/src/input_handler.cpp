#include "input_handler.hpp"
#include "armor_injection.hpp"
#include "categories.hpp"
#include "player_detection.hpp"
#include "shared_state.hpp"
#include "visibility_write.hpp"

#include <DetourModKit.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace EquipHide
{
    static DMK::Config::KeyComboList s_showAllCombos;
    static DMK::Config::KeyComboList s_hideAllCombos;

    void set_show_all_combos(DMK::Config::KeyComboList combos)
    {
        s_showAllCombos = std::move(combos);
    }

    void set_hide_all_combos(DMK::Config::KeyComboList combos)
    {
        s_hideAllCombos = std::move(combos);
    }

    static std::string combo_key(const DMK::Config::KeyComboList &combos)
    {
        std::string key;
        for (const auto &combo : combos)
        {
            for (const auto &mod : combo.modifiers)
                key += std::to_string(static_cast<int>(mod.source)) + ":" + std::to_string(mod.code) + "+";
            for (const auto &k : combo.keys)
                key += std::to_string(static_cast<int>(k.source)) + ":" + std::to_string(k.code) + ",";
            key += "|";
        }
        return key;
    }

    void flush_visibility() noexcept
    {
        update_hidden_mask();

        if (!flag_fallback_mode().load(std::memory_order_relaxed))
            resolve_player_vis_ctrls();

        // Reset injection flags so newly-toggled categories get entries created.
        auto &ps = player_state();
        for (int i = 0; i < k_maxProtagonists; ++i)
            ps.armorInjected[i].store(false, std::memory_order_relaxed);

        inject_armor_entries();
        apply_direct_vis_write();
    }

    void register_hotkeys()
    {
        auto &inputMgr = DMK::InputManager::get_instance();
        auto &states = category_states();
        auto &logger = DMK::Logger::get_instance();

        int toggleCount = 0;
        int showCount = 0;
        int hideCount = 0;
        int globalCount = 0;

        if (!s_showAllCombos.empty())
        {
            inputMgr.register_press(
                "ShowAll",
                s_showAllCombos,
                [&logger]()
                {
                    auto &st = category_states();
                    for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                        st[i].hidden.store(false, std::memory_order_relaxed);

                    logger.info("Equip hide: all categories VISIBLE");
                    flush_visibility();
                });
            ++globalCount;
        }

        if (!s_hideAllCombos.empty())
        {
            inputMgr.register_press(
                "HideAll",
                s_hideAllCombos,
                [&logger]()
                {
                    auto &st = category_states();
                    for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
                        st[i].hidden.store(true, std::memory_order_relaxed);

                    logger.info("Equip hide: all categories HIDDEN");
                    flush_visibility();
                });
            ++globalCount;
        }

        struct HotkeyGroup
        {
            DMK::Config::KeyComboList combos;
            std::vector<std::size_t> categoryIndices;
        };

        std::unordered_map<std::string, HotkeyGroup> groups;

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            if (!states[i].enabled.load(std::memory_order_relaxed))
                continue;
            if (states[i].toggleHotkeyCombos.empty())
                continue;

            auto &group = groups[combo_key(states[i].toggleHotkeyCombos)];
            if (group.combos.empty())
                group.combos = states[i].toggleHotkeyCombos;
            group.categoryIndices.push_back(i);
        }

        for (auto &[key, group] : groups)
        {
            std::string bindingName = "ToggleEquip";
            for (auto idx : group.categoryIndices)
                bindingName += std::string("_") + std::string(category_section(static_cast<Category>(idx)));

            auto indices = group.categoryIndices;

            inputMgr.register_press(
                bindingName,
                group.combos,
                [indices, &logger]()
                {
                    auto &st = category_states();
                    const bool newHidden = !st[indices[0]].hidden.load(std::memory_order_relaxed);
                    for (auto idx : indices)
                        st[idx].hidden.store(newHidden, std::memory_order_relaxed);

                    std::string catNames;
                    for (auto idx : indices)
                    {
                        if (!catNames.empty())
                            catNames += ", ";
                        catNames += category_section(static_cast<Category>(idx));
                    }
                    logger.info("Equip hide toggled [{}]: {}", catNames, newHidden ? "HIDDEN" : "VISIBLE");
                    flush_visibility();
                });

            ++toggleCount;
        }

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            if (!states[i].enabled.load(std::memory_order_relaxed))
                continue;

            const auto section = std::string(category_section(static_cast<Category>(i)));

            if (!states[i].showHotkeyCombos.empty())
            {
                inputMgr.register_press(
                    "ShowEquip_" + section,
                    states[i].showHotkeyCombos,
                    [i, &logger, section]()
                    {
                        category_states()[i].hidden.store(false, std::memory_order_relaxed);
                        logger.info("Equip hide [{}]: VISIBLE", section);
                        flush_visibility();
                    });
                ++showCount;
            }

            if (!states[i].hideHotkeyCombos.empty())
            {
                inputMgr.register_press(
                    "HideEquip_" + section,
                    states[i].hideHotkeyCombos,
                    [i, &logger, section]()
                    {
                        category_states()[i].hidden.store(true, std::memory_order_relaxed);
                        logger.info("Equip hide [{}]: HIDDEN", section);
                        flush_visibility();
                    });
                ++hideCount;
            }
        }

        const int total = globalCount + toggleCount + showCount + hideCount;
        logger.info("Hotkeys registered: {} binding(s) "
                    "({} toggle, {} show, {} hide, {} global)",
                    total, toggleCount, showCount, hideCount, globalCount);
    }

} // namespace EquipHide
