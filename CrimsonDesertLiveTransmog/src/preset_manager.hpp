#pragma once

#include "shared_state.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Transmog
{
    struct PresetSlot
    {
        bool active = false;
        // Runtime-only item id resolved from `itemName` against the
        // item catalog. Never persisted -- rebuilt on every load by
        // slot_from_json() or reresolve_all_names().
        uint16_t itemId = 0;
        // Stable game-data item name (e.g. "Kliff_PlateArmor_Helm").
        // The sole persistent identifier in the preset JSON. Resolved
        // to `itemId` at load time or after the deferred catalog scan.
        std::string itemName;
    };

    struct Preset
    {
        std::string name;
        std::array<PresetSlot, k_slotCount> slots{};
    };

    struct CharacterPresets
    {
        int activePreset = 0;
        std::vector<Preset> presets;
        /// Body-kind override used by the picker filter. One of:
        ///   "Auto"   (default) -- fall back to the hardcoded map
        ///                        (Kliff/Oongka = Male, Damiane = Female)
        ///   "Male" / "Female" / "Both"
        /// Lets body-swap mod users mark Kliff as "Female" so his
        /// picker shows the female-body-token pool. Persisted in
        /// presets.json per character.
        std::string bodyKind = "Auto";
    };

    class PresetManager
    {
    public:
        static PresetManager &instance();

        // --- File I/O ---

        bool load(const std::string &path);
        bool save() const;
        bool save(const std::string &path) const;

        // --- Character management ---

        std::vector<std::string> character_names() const;
        const std::string &active_character() const;
        void set_active_character(const std::string &name);

        /// Get/set the per-character body-kind override. Empty or
        /// "Auto" falls back to the hardcoded default in
        /// ItemNameTable::body_kind_for_character(). Values are saved
        /// to presets.json on set.
        std::string body_kind_of(const std::string &charName) const;
        void set_body_kind_of(const std::string &charName,
                              const std::string &bodyKind);

        // --- Preset management (operates on active character) ---

        int active_preset_index() const;
        int preset_count() const;
        const Preset *active_preset() const;
        Preset *active_preset_mut();
        const std::vector<Preset> &presets() const;

        /// Append a new preset from current slot_mappings state.
        void append_from_state();

        /// Overwrite the active preset with current slot_mappings state.
        void replace_current_from_state();

        /// Remove the active preset. Adjusts index to stay valid.
        void remove_current();

        /// Cycle to the next preset (wraps around). Applies to slot_mappings.
        void next_preset();

        /// Cycle to the previous preset (wraps around). Applies to slot_mappings.
        void prev_preset();

        /// Set active preset by index and apply to slot_mappings.
        void set_active_preset(int index);

        // --- State bridge ---

        /// Apply the active preset's slot data to the global slot_mappings.
        void apply_to_state() const;

        /// Capture current slot_mappings into a Preset.
        static Preset capture_from_state(const std::string &name = "");

        /**
         * @brief Re-resolve every loaded preset slot's itemName against
         *        the current ItemNameTable catalog.
         *
         * Called by the background deferred-scan thread once the item
         * catalog finishes building. Each slot with a non-empty
         * itemName is looked up fresh and its itemId populated. Slots
         * whose name is missing from the catalog are disabled
         * (active=false, itemId=0).
         *
         * @return The number of slots that were successfully resolved
         *         (for diagnostic logging).
         */
        std::size_t reresolve_all_names();

    private:
        PresetManager() = default;

        CharacterPresets &ensure_character(const std::string &name);

        std::map<std::string, CharacterPresets> m_characters;
        std::string m_activeCharacter = "Kliff";
        std::string m_filePath;
    };

} // namespace Transmog
