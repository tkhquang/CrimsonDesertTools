#ifndef TRANSMOG_PRESET_MANAGER_HPP
#define TRANSMOG_PRESET_MANAGER_HPP

#include "color_override/color_swatch_table.hpp"
#include "shared_state.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Transmog
{
    /**
     * @brief Maximum number of ARMOR_MOD records per slot.
     * @details The engine's dye record vector at dst+120 holds up to 16 channels. Most items use 2 to 5, and a cloak
     *          or a chest reaches higher. The UI exposes all 16. A channel above the item's natural count is a no-op.
     */
    inline constexpr std::size_t k_dyeChannelCount = 16;

    /**
     * @brief Per-channel dye override.
     * @details `group_hash` 0 means no override for this channel. Any other value injects an ARMOR_MOD record with
     *          R/G/B at +7/+8/+9.
     *
     *          `material_id` is the dye-template index (1..10) written at +4..+5 of the dye record. 0xFFFF selects the
     *          engine default, where the engine picks the natural variant for the item and channel combination. The
     *          engine resolves (item, channel) to a cat_code, then looks up the template's variant for that cat_code
     *          (per partprefabdyetexturepalleteinfo.pabgb).
     *
     *          `repair_byte` follows the engine's wear formula (`byte = ((100-pct)*127)/100`). 0 is pristine and 127
     *          is max wear. The engine also treats 0xFF as pristine, so a preset that recorded 0xFF round-trips: the
     *          slider reads 0xFF as 100% repair on load and a fresh slot saves the 0 default.
     *
     *          `group_name` is the data-file `_stringKey` that maps to `group_hash` (e.g. "Her_Color_Group_I"). It
     *          persists alongside the hash, so the right group survives a game update that renumbers the integer keys.
     */
    struct ChannelDye
    {
        std::uint32_t group_hash = 0;
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
        std::uint16_t material_id = 0xFFFF;
        std::uint8_t repair_byte = 0;
        std::string group_name; // empty = no fallback anchor

        bool active() const noexcept { return group_hash != 0; }
    };

    /**
     * @brief Per-slot dye state.
     * @details Index 0..15 maps to the ARMOR_MOD record idx at offset +6 of each 16-byte record. The array is sparse,
     *          because most slots populate only a few channels.
     */
    using SlotDyeChannels = std::array<ChannelDye, k_dyeChannelCount>;

    /// Reports whether any channel of the given slot dye is active.
    inline bool any_dye_active(const SlotDyeChannels &c) noexcept
    {
        for (const auto &ch : c)
            if (ch.active())
                return true;
        return false;
    }

    struct PresetSlot
    {
        bool active = false;
        // Runtime-only item id resolved from `itemName` against the item catalog. Never persisted - rebuilt on every
        // load by slot_from_json() or reresolve_all_names().
        uint16_t itemId = 0;
        // Stable game-data item name (e.g. "Kliff_PlateArmor_Helm"). The sole persistent identifier in the preset JSON.
        // Resolved to `itemId` at load time or after the deferred catalog scan.
        std::string itemName;
        // Optional body-mesh prefab name (e.g. "cd_nhw_no_ub_20027"). Empty when this slot has no body-mesh override.
        // apply_to_state resolves it against PrefabWrapperSwap::slot_catalog(). While the catalog is unpopulated
        // (boot heap walk still running) the resolution retries once the catalog finishes.
        std::string prefabName;
        // Per-channel dye overrides (16 channels max). Channels with group_hash == 0 are passed through to the engine
        // unchanged.
        SlotDyeChannels dye{};
        // Selects the dye-inject emission mode at apply time. Read by transmog_apply.cpp's apply paths and forwarded to
        // `DyeRecordInject::set_slot_dye_state(state, sparse)`.
        //
        //   sparse (true)  - emit ONLY channels with group_hash != 0. An inactive channel stays absent from the
        //     destination vector, so the engine paints its natural per-channel default there. Correct for a
        //     real-item capture and for most picker-curated dye.
        //   dense (false) - emit all `k_dyeChannelCount` records and fill each inactive channel with the first active
        //     channel's color. Required for an LT-fake or carrier transmog, where the engine holds no natural record
        //     to fall back on. Without the dense fill the descriptor's default palette wins and the fake renders
        //     colorless.
        //
        // It defaults to TRUE, so a freshly-captured outfit behaves as sparse. The dye popup exposes a per-slot
        // toggle for cross-class fake transmog, which needs the dense fallback.
        //
        // A preset saved before this field existed loads with `dye_sparse=false` (see slot_from_json), so its visuals
        // match the dense behavior it was saved under.
        bool dyeSparse = true;
    };

    struct Preset
    {
        std::string name;
        std::array<PresetSlot, k_slotCount> slots{};

        // ColorOverride (setter-substitute) persistence
        //
        // INDEPENDENT of `slots[].dye` - that path drives the engine's ARMOR_MOD record copier (`DyeRecordInject`)
        // keyed by dye-group + channel + RGB. The fields below persist the ColorOverride / SwatchTable path -
        // per-shader-property RGB overrides that `ColorOverride::SetterSubstitute` writes through to materials
        // outside the dye-record pipeline (monster carriers and the like). It serializes under the separate JSON key
        // `swatch_overrides`, so the two paths cannot collide on load.
        std::array<std::vector<ColorOverride::SwatchTable::PersistEntry>, k_slotCount> swatch_overrides{};

        // Captured-row palette: the (submesh, token) identities the slot captured, with no color values. It lets a
        // preset switch re-seed every row the user saw, even when the user picked colors for only a few. The engine
        // re-captures each row's default color live through capture_default_if_unset, so no persist is needed.
        //
        // Serialized as:
        //   "swatch_palette": { "Chest": { "submesh_X": ["_tok1","_tok2"] } }
        //
        // PersistEntry stores (submesh, token). The r/g/b fields stay unused here.
        std::array<std::vector<ColorOverride::SwatchTable::PersistEntry>, k_slotCount> swatch_palette{};

        // Per-slot master enable for ColorOverride substitution. Mirrors `ColorOverride::DyeSlot::slot_enabled`. A slot
        // with override entries but `swatch_slot_enabled[i] == false` keeps the rows in storage but disables the
        // substitute path - toggling the master flag back on resumes substitution without losing the user's
        // pixel-level picks.
        std::array<bool, k_slotCount> swatch_slot_enabled{};
    };

    struct CharacterPresets
    {
        int activePreset = 0;
        std::vector<Preset> presets;
        /**
         * @brief Body-kind override the picker filter reads.
         * @details One of "Auto" (the default, which falls back to the hardcoded map of Kliff and Oongka to Male and
         *          Damiane to Female), "Male", "Female" or "Both". A body-swap mod user marks Kliff as "Female" so his
         *          picker shows the female-body-token pool. It persists in presets.json per character.
         */
        std::string bodyKind = "Auto";

        /**
         * @brief Per-character UI preference for the ColorOverride dye picker.
         * @details When false, the default, the section shows only the summary "Recolor all" and "Same-default
         *          groups" rows. When true it shows every per-shader-property swatch row.
         */
        bool dyeAdvancedView = false;
    };

    class PresetManager
    {
    public:
        /// Returns the process-wide preset manager.
        static PresetManager &instance();

        // File I/O

        /**
         * @brief Loads every character's presets from @p path.
         * @param path UTF-8 presets.json path.
         * @return true when the file parsed. A missing file starts from defaults and also returns true.
         */
        bool load(const std::string &path);

        /**
         * @brief Saves every character's presets to the path the last load used.
         * @return true when the write completed.
         */
        bool save() const;

        /**
         * @brief Saves every character's presets to @p path.
         * @param path UTF-8 presets.json path.
         * @return true when the write completed.
         */
        bool save(const std::string &path) const;

        // Character management
        //
        // Three character axes drive the apply pipeline, and each one answers a different question:
        //
        //   1. CONTROLLED - the character the player drives in-game. It selects carrier defaults, body-mesh wrapper
        //      sources and the stale-body guard. m_controlledCharacter and active_character() hold it, and
        //      load_detect_thread updates it on a radial swap or a save-load.
        //   2. EDITING - the character whose preset list the overlay shows. m_editingCharacter and
        //      editing_character() hold it, and the UI updates it on a dropdown change. It follows CONTROLLED until
        //      the user picks another character, which "pins" it until the user unpins or re-selects the controlled
        //      character.
        //   3. APPLY TARGET - the body the next apply installs its carrier and fake on. It equals EDITING while the
        //      pin is engaged, and CONTROLLED otherwise. current_apply_owner() below is the only correct reader.
        //
        // With editing pinned, the apply pipeline reads target itemIds from the editing character's preset while the
        // carriers and the source body still come from the controlled character. That is the cross-body "wear another
        // character's preset" path.
        //
        // @warning The load-detect thread writes the controlled axis and the overlay render thread writes the editing
        //          axis. Neither read is synchronized.

        /**
         * @brief Lists every character that owns a preset entry.
         * @return The character names, in map order.
         */
        std::vector<std::string> character_names() const;

        /// Returns the controlled character.
        const std::string &active_character() const;

        /**
         * @brief Sets the controlled character.
         * @param name Character the player now drives.
         * @details An unpinned editing axis follows. If the player now controls the character the user pinned for
         *          editing, the pin auto-clears, because it no longer names anything distinct.
         */
        void set_active_character(const std::string &name);

        /// Returns the character whose preset list the overlay edits.
        const std::string &editing_character() const;

        /**
         * @brief Sets the editing character.
         * @param name Character the overlay dropdown selected.
         * @details The pin engages whenever the new editing character differs from controlled. A selection of the
         *          controlled character clears the pin.
         */
        void set_editing_character(const std::string &name);

        /// Reports whether the editing axis is pinned away from the controlled character.
        bool editing_pinned() const noexcept;

        /// Drops the pin and snaps editing back to controlled.
        void clear_editing_pin();

        /**
         * @brief Reads the per-character body-kind override.
         * @param charName Character to query.
         * @return The stored value, or "Auto" when the character carries no override.
         * @details Empty or "Auto" falls back to the default in ItemNameTable::body_kind_for_character().
         */
        std::string body_kind_of(const std::string &charName) const;

        /**
         * @brief Writes the per-character body-kind override.
         * @param charName Character to update.
         * @param bodyKind One of "Auto", "Male", "Female" or "Both". Any other value collapses to "Auto".
         * @details The new value is saved to presets.json.
         */
        void set_body_kind_of(const std::string &charName, const std::string &bodyKind);

        // Preset management (operates on active character)

        /// Returns the editing character's active preset index, clamped into the stored list.
        int active_preset_index() const;

        /// Returns how many presets the editing character owns.
        int preset_count() const;

        /// Returns the editing character's active preset, or nullptr when the character owns none.
        const Preset *active_preset() const;

        /// Mutable form of @ref active_preset.
        Preset *active_preset_mut();

        /**
         * @brief Mutable active preset, minted from current state when the editing character owns none.
         * @return The preset to write to, or nullptr on an unexpected failure.
         * @details Callers such as the Dye picker always need a preset to write to rather than a silent no-op. A
         *          minted preset persists through save(). It mirrors the auto-create branch of
         *          replace_current_from_state().
         */
        Preset *active_preset_mut_or_create();

        /// Returns the editing character's full preset list.
        const std::vector<Preset> &presets() const;

        /**
         * @brief Appends a fresh preset with the five armor slots ticked and set to none.
         * @details Helm, Chest, Cloak, Gloves and Boots hide their armor piece. Gear and accessory slots stay
         *          unticked, so the new preset hides no working item, such as a lit lantern. It pushes the new state
         *          into slot_mappings before it returns, so the caller can call manual_apply() at once.
         */
        void append_from_state();

        /**
         * @brief Appends a clone of the active preset's in-memory state (items, dye, swatches).
         * @details It ignores unsaved edits in slot_mappings, so the clone matches the source as a reload from disk
         *          produces it. Use it for a clean fork that leaves the original untouched.
         */
        void duplicate_current();

        /**
         * @brief Appends a preset that captures the current pending state.
         * @details The capture covers slot_mappings plus the in-place dye and swatch edits on the active preset, and
         *          it overwrites none of the active preset's saved item rows. Use it to fork-save mid-edit, for
         *          example to move a new helmet pick into its own preset and leave the source rows alone.
         */
        void save_as_new_from_state();

        /// Overwrites the active preset with the current slot_mappings state.
        void replace_current_from_state();

        /// Removes the active preset and clamps the index back into range.
        void remove_current();

        /// Cycles to the next preset, wraps at the end, and applies it to slot_mappings.
        void next_preset();

        /// Cycles to the previous preset, wraps at the start, and applies it to slot_mappings.
        void prev_preset();

        /**
         * @brief Selects a preset by index and applies it to slot_mappings.
         * @param index Preset index, clamped into the stored list.
         */
        void set_active_preset(int index);

        // State bridge

        /// Applies the active preset's slot data to the global slot_mappings.
        void apply_to_state() const;

        /**
         * @brief Re-seeds ColorOverride placeholder rows for slots whose saved JSON carries entries the live
         *        SwatchTable lacks.
         * @details populate_from_persisted resolves each saved entry's token name through
         *          TokenTable::token_id_for_name, which returns 0 until the AOB-discovered token table or the runtime
         *          interner hook observes that token. On a cold game load auto_reinit_from runs once at
         *          PresetManager::load time, so a token unknown at that moment drops silently, the picker rows never
         *          seed, and the overlay's per-slot override chip never renders (g_count stays 0). The engine still
         *          applies the saved RGB through the PendingOverrides path, so the visuals are correct while the UI
         *          reads empty until the user triggers a preset switch.
         *
         *          This method is the lazy retry. On every overlay frame it re-runs populate_from_persisted for any
         *          slot whose live table is empty and whose preset holds saved entries. populate_from_persisted is
         *          idempotent, because an already-seeded entry short-circuits through find_seeded, so the call costs
         *          nothing once every token resolves. The walk is bounded by k_slotCount times the saved-entry count
         *          and touches only slots that wait on token resolution.
         */
        void reseed_unresolved_persisted_swatches() const;

        /**
         * @brief Looks up the active preset of an arbitrary character without moving either character axis.
         * @param charName Character to read.
         * @return That character's active preset, or nullptr when the character is unknown or owns no preset.
         * @details The body-mesh prefab picker uses it to borrow the Kairos default-carrier preset's slot itemIds
         *          while the user edits on a different character.
         */
        const Preset *active_preset_of(const std::string &charName) const;

        /**
         * @brief Captures the current slot_mappings into a Preset.
         * @param name Name for the captured preset.
         * @return The captured preset.
         */
        static Preset capture_from_state(const std::string &name = "");

        /**
         * @brief Re-resolve every loaded preset slot's itemName against the current ItemNameTable catalog.
         *
         * @details The background deferred-scan thread calls it once the item catalog finishes its build. Each slot
         *          with a non-empty itemName gets a fresh lookup and its itemId. A slot whose name is missing from the
         *          catalog is disabled (active=false, itemId=0).
         *
         * @return The number of slots that resolved, for diagnostic logging.
         */
        std::size_t reresolve_all_names();

    private:
        PresetManager() = default;

        CharacterPresets &ensure_character(const std::string &name);

        // Snapshot of the active preset's dye state at the point it was last loaded or saved. Used to revert in-memory
        // dye edits when the user switches presets without saving - dye edits are visually live (auto-apply), but the
        // JSON commit only happens on Save. Switching/cycling presets discards uncommitted dye changes via
        // revert_dye_snapshot. Mutable so save() can capture without losing const.
        mutable std::array<SlotDyeChannels, k_slotCount> m_dyeSnapshot{};
        mutable bool m_dyeSnapshotValid = false;

        // Captures current active preset's dye into m_dyeSnapshot.
        void capture_dye_snapshot() const noexcept;

        // If dye_dirty() is set AND a snapshot is valid, copies the snapshot back over the active preset's dye, undoing
        // every mutation made since the last load/save. Then clears the dirty flag. Call before switching presets.
        void revert_active_dye_to_snapshot() noexcept;

        // Rotates the editing target from m_editingCharacter to `new_name`: snapshots outgoing preset's ColorOverride
        // swatch state, resets the live swatch tables, revert+update+ capture the dye snapshot, then restores swatches
        // + auto-reinits slots from the incoming preset. The caller is responsible for any ensure_character / pin
        // bookkeeping.
        // Pre-condition: new_name != m_editingCharacter.
        void rotate_editing_target_to(const std::string &new_name);

        std::map<std::string, CharacterPresets> m_characters;
        // See class-level comment for the controlled / editing split.
        std::string m_controlledCharacter = "Kliff";
        // The load-detect thread rotates this axis through set_active_character. A method that mutates preset state
        // and then calls apply_to_state must therefore snapshot it into a local ONCE and resolve everything from that
        // local, or the mutation lands on one character and the apply on another.
        std::string m_editingCharacter = "Kliff";
        bool m_editingPinned = false;
        std::string m_filePath;
    };

    /**
     * @brief Returns the name of the character whose body the next apply lands on.
     * @return The editing character while the editing pin is engaged, the controlled character otherwise.
     * @details Every caller that asks "whose carrier do I install" or "whose row in the per-char trackers do I read"
     *          must consult this helper rather than re-derive the ternary. The result is a reference into
     *          PresetManager's internal strings. It is safe within one thread of execution. Do not store the
     *          reference across a call that can mutate either axis.
     */
    [[nodiscard]] const std::string &current_apply_owner() noexcept;

} // namespace Transmog

#endif // TRANSMOG_PRESET_MANAGER_HPP
