#include "preset_manager.hpp"
#include "constants.hpp"
#include "item_name_table.hpp"
#include "transmog.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>
#include <nlohmann/json.hpp>

#include <fstream>

using json = nlohmann::json;

namespace Transmog
{
    // --- JSON serialization ---

    static json slot_to_json(const PresetSlot &s)
    {
        return json{
            {"active", s.active},
            {"itemName", s.itemName},
        };
    }

    static PresetSlot slot_from_json(const json &j)
    {
        PresetSlot s;
        s.active = j.value("active", false);
        s.itemName = j.value("itemName", std::string());

        if (s.itemName.empty())
            return s;

        // itemName is the sole persistent identifier. If the catalog
        // is already built we resolve immediately; otherwise itemId
        // stays 0 until reresolve_all_names() runs after the deferred
        // scan completes.
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
            return s;

        auto resolved = table.id_of(s.itemName);
        if (resolved.has_value())
        {
            s.itemId = *resolved;
        }
        else
        {
            DMK::Logger::get_instance().warning(
                "[preset] item name '{}' not in current catalog — "
                "slot disabled; re-pick in the overlay",
                s.itemName);
            s.active = false;
        }
        return s;
    }

    static json preset_to_json(const Preset &p)
    {
        json slotsArr = json::array();
        for (std::size_t i = 0; i < k_slotCount; ++i)
            slotsArr.push_back(slot_to_json(p.slots[i]));

        return json{{"name", p.name}, {"slots", slotsArr}};
    }

    static Preset preset_from_json(const json &j)
    {
        Preset p;
        p.name = j.value("name", std::string("Unnamed"));

        if (j.contains("slots") && j["slots"].is_array())
        {
            auto &arr = j["slots"];
            for (std::size_t i = 0; i < k_slotCount && i < arr.size(); ++i)
                p.slots[i] = slot_from_json(arr[i]);
        }
        return p;
    }

    static json character_to_json(const CharacterPresets &cp)
    {
        json presetsArr = json::array();
        for (auto &p : cp.presets)
            presetsArr.push_back(preset_to_json(p));

        return json{{"activePreset", cp.activePreset}, {"presets", presetsArr}};
    }

    static CharacterPresets character_from_json(const json &j)
    {
        CharacterPresets cp;
        cp.activePreset = j.value("activePreset", 0);

        if (j.contains("presets") && j["presets"].is_array())
        {
            for (auto &pj : j["presets"])
                cp.presets.push_back(preset_from_json(pj));
        }

        // Clamp active index.
        if (!cp.presets.empty())
            cp.activePreset = std::clamp(cp.activePreset, 0,
                                         static_cast<int>(cp.presets.size()) - 1);
        else
            cp.activePreset = 0;

        return cp;
    }

    // --- PresetManager ---

    PresetManager &PresetManager::instance()
    {
        static PresetManager s_instance;
        return s_instance;
    }

    bool PresetManager::load(const std::string &path)
    {
        auto &logger = DMK::Logger::get_instance();
        m_filePath = path;

        std::ifstream file(path);
        if (!file.is_open())
        {
            logger.info("Presets file not found — starting with defaults");

            // Create default entries for known characters.
            ensure_character("Kliff");
            ensure_character("Damiane");
            ensure_character("Oongka");

            return true;
        }

        try
        {
            json root = json::parse(file);

            m_activeCharacter = root.value("activeCharacter", std::string("Kliff"));

            if (root.contains("characters") && root["characters"].is_object())
            {
                for (auto &[name, cj] : root["characters"].items())
                    m_characters[name] = character_from_json(cj);
            }

            // Ensure known characters exist even if not in the file.
            ensure_character("Kliff");
            ensure_character("Damiane");
            ensure_character("Oongka");

            logger.info("Presets loaded: {} character(s) from '{}'",
                        m_characters.size(), path);

            if (!ItemNameTable::instance().ready())
            {
                logger.info(
                    "[preset] item catalog not ready at load time — name "
                    "resolution deferred until background scan completes");
            }
        }
        catch (const json::exception &e)
        {
            logger.warning("Failed to parse presets file: {}", e.what());
            ensure_character("Kliff");
            ensure_character("Damiane");
            ensure_character("Oongka");
            return false;
        }

        return true;
    }

    bool PresetManager::save() const
    {
        return save(m_filePath);
    }

    bool PresetManager::save(const std::string &path) const
    {
        auto &logger = DMK::Logger::get_instance();

        json root;
        // Schema v3: slots carry only itemName (stable identifier).
        // itemId is resolved at runtime from the item catalog.
        root["version"] = 3;
        root["activeCharacter"] = m_activeCharacter;

        json chars = json::object();
        for (auto &[name, cp] : m_characters)
            chars[name] = character_to_json(cp);

        root["characters"] = chars;

        std::ofstream file(path);
        if (!file.is_open())
        {
            logger.warning("Failed to write presets to '{}'", path);
            return false;
        }

        file << root.dump(2);
        logger.info("Presets saved to '{}'", path);
        return true;
    }

    // --- Character management ---

    std::vector<std::string> PresetManager::character_names() const
    {
        std::vector<std::string> names;
        names.reserve(m_characters.size());
        for (auto &[name, _] : m_characters)
            names.push_back(name);
        return names;
    }

    const std::string &PresetManager::active_character() const
    {
        return m_activeCharacter;
    }

    void PresetManager::set_active_character(const std::string &name)
    {
        m_activeCharacter = name;
        ensure_character(name);
    }

    // --- Preset management ---

    int PresetManager::active_preset_index() const
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end())
            return 0;
        return it->second.activePreset;
    }

    int PresetManager::preset_count() const
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end())
            return 0;
        return static_cast<int>(it->second.presets.size());
    }

    const Preset *PresetManager::active_preset() const
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.activePreset, 0,
                              static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    Preset *PresetManager::active_preset_mut()
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return nullptr;

        auto idx = std::clamp(it->second.activePreset, 0,
                              static_cast<int>(it->second.presets.size()) - 1);
        return &it->second.presets[static_cast<std::size_t>(idx)];
    }

    const std::vector<Preset> &PresetManager::presets() const
    {
        static const std::vector<Preset> s_empty;
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end())
            return s_empty;
        return it->second.presets;
    }

    void PresetManager::append_from_state()
    {
        auto &cp = ensure_character(m_activeCharacter);
        int idx = static_cast<int>(cp.presets.size());
        std::string name = "Preset " + std::to_string(idx);
        cp.presets.push_back(capture_from_state(name));
        cp.activePreset = idx;

        DMK::Logger::get_instance().info("Preset appended: '{}' (index {})", name, idx);
        save();
    }

    void PresetManager::replace_current_from_state()
    {
        auto *p = active_preset_mut();
        if (!p)
        {
            append_from_state();
            return;
        }

        auto captured = capture_from_state(p->name);
        p->slots = captured.slots;

        DMK::Logger::get_instance().info("Preset replaced: '{}'", p->name);
        save();
    }

    void PresetManager::remove_current()
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        auto &cp = it->second;
        auto idx = std::clamp(cp.activePreset, 0,
                              static_cast<int>(cp.presets.size()) - 1);

        DMK::Logger::get_instance().info("Preset removed: '{}'",
                                         cp.presets[static_cast<std::size_t>(idx)].name);

        cp.presets.erase(cp.presets.begin() + idx);

        if (cp.presets.empty())
            cp.activePreset = 0;
        else
            cp.activePreset = std::min(idx, static_cast<int>(cp.presets.size()) - 1);

        save();
    }

    void PresetManager::next_preset()
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        auto &cp = it->second;
        cp.activePreset = (cp.activePreset + 1) % static_cast<int>(cp.presets.size());

        DMK::Logger::get_instance().info(
            "Preset cycled to: '{}' ({}/{})",
            cp.presets[static_cast<std::size_t>(cp.activePreset)].name,
            cp.activePreset + 1, cp.presets.size());

        apply_to_state();
        save();
    }

    void PresetManager::prev_preset()
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        auto &cp = it->second;
        int count = static_cast<int>(cp.presets.size());
        cp.activePreset = (cp.activePreset - 1 + count) % count;

        DMK::Logger::get_instance().info(
            "Preset cycled to: '{}' ({}/{})",
            cp.presets[static_cast<std::size_t>(cp.activePreset)].name,
            cp.activePreset + 1, cp.presets.size());

        apply_to_state();
        save();
    }

    void PresetManager::set_active_preset(int index)
    {
        auto it = m_characters.find(m_activeCharacter);
        if (it == m_characters.end() || it->second.presets.empty())
            return;

        auto &cp = it->second;
        cp.activePreset = std::clamp(index, 0,
                                     static_cast<int>(cp.presets.size()) - 1);
        apply_to_state();
    }

    // --- State bridge ---

    void PresetManager::apply_to_state() const
    {
        const auto *p = active_preset();
        if (!p)
            return;

        auto &mappings = slot_mappings();

        // NOTE: must NOT touch last_applied_ids() here. lastIds tracks what
        // apply_all_transmog has actively injected into the game — the diff
        // logic depends on lastIds holding the PREVIOUS applied state so it
        // can compute drop-masks on preset switching. Writing to lastIds
        // here would pre-populate it with the NEW preset values before the
        // apply runs, breaking drop detection.
        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            mappings[i].active = p->slots[i].active;
            mappings[i].targetItemId = p->slots[i].itemId;
        }
    }

    Preset PresetManager::capture_from_state(const std::string &name)
    {
        Preset p;
        p.name = name.empty() ? "Captured" : name;

        auto &mappings = slot_mappings();
        const auto &table = ItemNameTable::instance();

        for (std::size_t i = 0; i < k_slotCount; ++i)
        {
            p.slots[i].active = mappings[i].active;
            p.slots[i].itemId = mappings[i].targetItemId;
            // id 0 means "none" — don't resolve a name for it.
            // The catalog's entry at index 0 is a real item
            // (Pyeonjeon_Arrow) and saving that name would cause
            // the preset to load an unintended item on reresolve.
            if (table.ready() && mappings[i].targetItemId != 0)
                p.slots[i].itemName = table.name_of(mappings[i].targetItemId);
            else
                p.slots[i].itemName.clear();
        }
        return p;
    }

    std::size_t PresetManager::reresolve_all_names()
    {
        auto &logger = DMK::Logger::get_instance();
        const auto &table = ItemNameTable::instance();
        if (!table.ready())
        {
            logger.warning(
                "[preset] reresolve_all_names called before catalog ready — "
                "no-op");
            return 0;
        }

        std::size_t resolved = 0;
        std::size_t disabled = 0;

        for (auto &[charName, cp] : m_characters)
        {
            for (auto &preset : cp.presets)
            {
                for (auto &slot : preset.slots)
                {
                    if (slot.itemName.empty())
                        continue;

                    auto fresh = table.id_of(slot.itemName);
                    if (fresh.has_value())
                    {
                        slot.itemId = *fresh;
                        ++resolved;
                    }
                    else
                    {
                        logger.warning(
                            "[preset] '{}' not in catalog — disabling "
                            "(char='{}' preset='{}')",
                            slot.itemName, charName, preset.name);
                        slot.itemId = 0;
                        slot.active = false;
                        ++disabled;
                    }
                }
            }
        }

        logger.info(
            "[preset] reresolve complete: {} resolved, {} disabled",
            resolved, disabled);
        return resolved;
    }

    // --- Private ---

    CharacterPresets &PresetManager::ensure_character(const std::string &name)
    {
        return m_characters[name]; // Default-constructs if absent.
    }

} // namespace Transmog
