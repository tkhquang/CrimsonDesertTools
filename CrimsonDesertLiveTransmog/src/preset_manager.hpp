#pragma once

#include "shared_state.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Transmog
{
    /// Maximum number of ARMOR_MOD records per slot. Engine's dye
    /// record vector at dst+120 holds up to 16 channels; most items
    /// use 2-5, but cloak/chest can extend higher. We expose all 16
    /// to the user; channels above the item's natural count are no-ops.
    inline constexpr std::size_t k_dyeChannelCount = 16;

    /// Per-channel dye override:
    ///   group_hash = 0  -> no override for this channel
    ///   else            -> inject ARMOR_MOD record with R/G/B at +7/+8/+9
    /// `material_id` is the dye-template index (1..10) written at
    /// +4..+5 of the dye record. 0xFFFF = engine default (the engine
    /// picks the natural variant for the item/channel combo). The
    /// engine resolves (item, channel) -> cat_code, then looks up the
    /// template's variant for that cat_code (per
    /// partprefabdyetexturepalleteinfo.pabgb).
    /// `repair_byte` follows the engine's wear formula
    /// (`byte = ((100-pct)*127)/100`); 0 = pristine, 127 = max wear.
    /// The engine still treats the legacy 0xFF as pristine, so old
    /// presets that recorded 0xFF round-trip cleanly: the slider
    /// reads 0xFF as 100% repair on load, and a fresh slot saves the
    /// new 0 default instead.
    /// `group_name` is the data-file `_stringKey` that maps to
    /// `group_hash` (e.g. "Her_Color_Group_I"). Persisted alongside
    /// the hash so we can recover the right group across game updates
    /// even if Pearl Abyss renumbers the integer keys.
    struct ChannelDye
    {
        std::uint32_t group_hash = 0;
        std::uint8_t  r = 0;
        std::uint8_t  g = 0;
        std::uint8_t  b = 0;
        std::uint16_t material_id = 0xFFFF;
        std::uint8_t  repair_byte = 0;
        std::string   group_name; // empty = no fallback anchor

        bool active() const noexcept { return group_hash != 0; }
    };

    /// Per-slot dye state. Index 0..15 maps to ARMOR_MOD record idx
    /// at offset +6 of each 16-byte record. Sparse: most slots only
    /// have a few channels populated.
    using SlotDyeChannels = std::array<ChannelDye, k_dyeChannelCount>;

    /// True if any channel of the given slot dye is active.
    inline bool any_dye_active(const SlotDyeChannels &c) noexcept
    {
        for (const auto &ch : c)
            if (ch.active()) return true;
        return false;
    }

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
        // Optional body-mesh prefab name (e.g. "cd_nhw_no_ub_20027").
        // Empty when this slot has no body-mesh override. Resolved
        // against PrefabWrapperSwap::slot_catalog() in apply_to_state;
        // if the catalog isn't yet populated (boot heap walk still
        // running) the resolution retries when the catalog finishes.
        std::string prefabName;
        // Per-channel dye overrides (16 channels max). Channels with
        // group_hash == 0 are passed through to the engine unchanged.
        SlotDyeChannels dye{};
        // Selects the dye-inject emission mode at apply time. Read
        // by transmog_apply.cpp's apply paths and forwarded to
        // `DyeRecordInject::set_slot_dye_state(state, sparse)`.
        //
        //   sparse (true)  -- emit ONLY channels with group_hash != 0;
        //     inactive channels stay absent from the destination
        //     vector so the engine paints its natural per-channel
        //     defaults on the rest. Correct for real-item captures
        //     and for most picker-curated dye.
        //   dense  (false) -- emit all `k_dyeChannelCount` records,
        //     filling inactive channels with the first active
        //     channel's colour. Required for LT-fake / carrier
        //     transmog where the engine has no natural records to
        //     fall back on; without the dense fill the descriptor's
        //     default palette wins and the fake stays uncoloured.
        //
        // Defaults to TRUE so freshly-captured outfits behave as
        // sparse out of the box. The dye popup exposes a per-slot
        // toggle for users doing cross-class fake transmog where
        // the dense fallback is wanted.
        //
        // Backward compat: presets saved before this field existed
        // load with `dye_sparse=false` (see slot_from_json) so their
        // visuals match the dense behaviour they were saved under.
        bool dyeSparse = true;
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

        /// Append a fresh preset with every slot ticked + none (hide all).
        /// Pushes the new state into slot_mappings before returning so the
        /// caller can manual_apply() immediately.
        void append_from_state();

        /// Append a new preset that snapshots the current slot_mappings
        /// (active character's picker rows). Unlike append_from_state, this
        /// preserves the currently-loaded item selections so the user can
        /// fork-edit an existing preset without overwriting the source.
        void duplicate_current();

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

        /// Look up the *active* preset for an arbitrary character without
        /// changing the manager's currently-selected character. Used by
        /// the body-mesh prefab picker to "borrow" the Kairos (default-
        /// carrier) preset's slot itemIds when the user is editing on a
        /// different character. Returns nullptr if the character is
        /// unknown or has no presets.
        const Preset *active_preset_of(const std::string &charName) const;

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

        // Snapshot of the active preset's dye state at the point it
        // was last loaded or saved. Used to revert in-memory dye
        // edits when the user switches presets without saving --
        // dye edits are visually live (auto-apply), but the JSON
        // commit only happens on Save. Switching/cycling presets
        // discards uncommitted dye changes via revert_dye_snapshot.
        // Mutable so save() can capture without losing const.
        mutable std::array<SlotDyeChannels, k_slotCount> m_dyeSnapshot{};
        mutable bool m_dyeSnapshotValid = false;

        // Captures current active preset's dye into m_dyeSnapshot.
        void capture_dye_snapshot() const noexcept;

        // If dye_dirty() is set AND a snapshot is valid, copies the
        // snapshot back over the active preset's dye, undoing every
        // mutation made since the last load/save. Then clears the
        // dirty flag. Call before switching presets.
        void revert_active_dye_to_snapshot() noexcept;

        std::map<std::string, CharacterPresets> m_characters;
        std::string m_activeCharacter = "Kliff";
        std::string m_filePath;
    };

} // namespace Transmog
