#pragma once

#include "color_override/color_swatch_table.hpp"
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

        // ---- ColorOverride (setter-substitute) persistence -----------
        //
        // INDEPENDENT of `slots[].dye` -- that path drives the engine's
        // ARMOR_MOD record copier (`DyeRecordInject`) keyed by
        // dye-group + channel + RGB. The fields below persist the
        // ColorOverride / SwatchTable path -- per-shader-property
        // RGB overrides that `ColorOverride::SetterSubstitute` writes
        // through to materials that don't go through the dye-record
        // pipeline (monster carriers, etc.). Serialized under the
        // separate JSON key `swatch_overrides` so the two paths
        // can't collide on load.
        std::array<std::vector<ColorOverride::SwatchTable::PersistEntry>,
                   k_slotCount> swatch_overrides{};

        // Captured-row palette: just the (submesh, token) identities
        // the slot has captured, no colour values. Lets us re-seed
        // every row the user has seen on a preset switch, even when
        // they only picked colours for a handful. The actual default
        // colour for each row is re-captured live by the engine via
        // capture_default_if_unset -- no need to persist it.
        //
        // Serialized as:
        //   "swatch_palette": { "Chest": { "submesh_X": ["_tok1","_tok2"] } }
        //
        // PersistEntry stores (submesh, token); r/g/b is unused here.
        std::array<std::vector<ColorOverride::SwatchTable::PersistEntry>,
                   k_slotCount> swatch_palette{};

        // Per-slot master enable for ColorOverride substitution. Mirrors
        // `ColorOverride::DyeSlot::slot_enabled`. A slot with override
        // entries but `swatch_slot_enabled[i] == false` keeps the rows
        // in storage but disables the substitute path -- toggling the
        // master flag back on resumes substitution without losing the
        // user's pixel-level picks.
        std::array<bool, k_slotCount> swatch_slot_enabled{};
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

        /// Per-character UI preference for the ColorOverride dye
        /// picker: when false (default) the section shows only the
        /// summary "Recolor all" + "Same-default groups" rows; when
        /// true it shows every per-shader-property swatch row.
        bool dyeAdvancedView = false;
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
        //
        // The manager tracks two distinct character identities:
        //
        //   controlled = the in-game character the player is currently
        //                controlling. Drives carrier defaults, body-
        //                mesh wrapper source resolution, and the stale-
        //                body guard. Owned by the load-detect thread
        //                via set_active_character().
        //
        //   editing    = the character whose preset list the overlay
        //                is showing and the user is mutating. By
        //                default it tracks `controlled`. When the user
        //                picks a different character in the overlay
        //                dropdown, `editing` becomes "pinned" and
        //                stops following `controlled` until the user
        //                unpins (manually, or by re-selecting the
        //                controlled character). With editing pinned,
        //                the apply pipeline reads target itemIds from
        //                the editing character's preset while carriers
        //                and the source body still come from the
        //                controlled character; this is the cross-body
        //                "wear another character's preset" path.
        //
        // active_character() / set_active_character() always refer to
        // the *controlled* axis. editing_character() and friends are
        // the UI-side API.
        //
        // Three character axes drive the apply pipeline. Mixing them
        // is the cause of every "wrong char rendered" bug LT has hit
        // since multi-character support landed; resist the urge to
        // read "active_character" inside an apply path just because
        // it's the shortest name:
        //
        //   1. CONTROLLED -- whoever the player is driving in-game.
        //      Tracked by m_controlledCharacter / active_character().
        //      Updated by load_detect_thread on radial swap / save-
        //      load.
        //   2. EDITING -- whoever the user picked in the overlay
        //      dropdown. Tracked by m_editingCharacter /
        //      editing_character(). Updated by the UI on dropdown
        //      change.
        //   3. APPLY TARGET -- whose body the next apply will install
        //      its carrier+fake on. Equals EDITING when the pin is
        //      engaged (editing != controlled), CONTROLLED otherwise.
        //      Use current_apply_owner() (below) -- never inline the
        //      ternary at a call site, or the next refactor will miss
        //      one.
        //
        // Thread safety: both axes are written on different threads
        // (load-detect writes controlled; the overlay render thread
        // writes editing). PresetManager has no internal locking;
        // callers rely on the fact that std::string assignment of a
        // small known-size value is atomic enough in practice for the
        // single-string read on the apply path, and that pin/unpin
        // transitions are immediately followed by a slot_mappings
        // reset + apply, which serialises any half-observed state.

        std::vector<std::string> character_names() const;

        /// Returns the controlled character.
        const std::string &active_character() const;
        /// Sets the controlled character. If editing is unpinned,
        /// editing follows. If editing was pinned and the user has
        /// switched to controlling the very character they had pinned
        /// for editing, the pin auto-clears (the pin no longer
        /// represents anything distinct).
        void set_active_character(const std::string &name);

        /// Returns the character whose preset list the overlay is
        /// editing.
        const std::string &editing_character() const;
        /// Sets the editing character. The pin engages whenever the
        /// new editing character differs from controlled; selecting
        /// the controlled character clears the pin.
        void set_editing_character(const std::string &name);
        bool editing_pinned() const noexcept;
        /// Drop the pin and snap editing back to controlled.
        void clear_editing_pin();

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

        /// Append a new preset that is a pure clone of the active
        /// preset's in-memory state (items, dye, swatches). Ignores any
        /// unsaved edits in slot_mappings -- the clone reflects what the
        /// source preset would look like if reloaded from disk. Use when
        /// you want a clean fork to alter without touching the original.
        void duplicate_current();

        /// Append a new preset that captures the current pending state
        /// (slot_mappings + in-place dye/swatch on the active preset)
        /// without overwriting the active preset's saved item rows.
        /// Use to fork-save mid-edit (e.g. after picking a new helmet
        /// on the active preset, save those edits to a new preset and
        /// leave the source's saved rows untouched).
        void save_as_new_from_state();

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

        /// Re-seed ColorOverride placeholder rows for slots whose saved
        /// JSON has entries but whose live SwatchTable is still empty
        /// for the active preset.
        ///
        /// Why this exists: populate_from_persisted resolves each saved
        /// entry's token name via TokenTable::token_id_for_name, which
        /// returns 0 until either the AOB-discovered token table or the
        /// runtime interner hook has seen that token. On a cold game
        /// load auto_reinit_from runs once at PresetManager::load time;
        /// any saved tokens not yet known at that point get dropped
        /// silently, the picker rows never seed, and the overlay's
        /// per-slot override chip never renders (g_count stays 0). The
        /// engine itself still applies the saved RGB through the
        /// PendingOverrides path, so visuals are correct but the UI
        /// looks empty until the user manually triggers a preset
        /// switch.
        ///
        /// This method is the lazy retry: on every overlay frame it
        /// re-runs populate_from_persisted for any slot whose live
        /// table is empty but whose preset has saved entries. The
        /// underlying populate_from_persisted is idempotent (already-
        /// seeded entries short-circuit via find_seeded), so the call
        /// is a no-op once every token has resolved. Cheap walk
        /// otherwise: bounded by k_slotCount * saved-entry-count and
        /// only touches slots that are actually waiting on token
        /// resolution.
        void reseed_unresolved_persisted_swatches() const;

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

        // Rotates the editing target from m_editingCharacter to
        // `new_name`: snapshots outgoing preset's ColorOverride
        // swatch state, resets the live swatch tables, revert+update+
        // capture the dye snapshot, then restores swatches + auto-
        // reinits slots from the incoming preset. The caller is
        // responsible for any ensure_character / pin bookkeeping.
        // Pre-condition: new_name != m_editingCharacter.
        void rotate_editing_target_to(const std::string &new_name);

        std::map<std::string, CharacterPresets> m_characters;
        // See class-level comment for the controlled / editing split.
        std::string m_controlledCharacter = "Kliff";
        std::string m_editingCharacter    = "Kliff";
        bool        m_editingPinned       = false;
        std::string m_filePath;
    };

    /// Returns the name of the character whose body the next apply
    /// SHOULD land on. This is the editing character when the editing
    /// pin is engaged (the user has dropped the dropdown onto a
    /// non-controlled char), and the controlled character otherwise.
    /// Every place that needs to ask "whose carrier do I install" or
    /// "whose row in the per-char trackers do I read" should consult
    /// this helper rather than re-deriving the ternary in-line.
    ///
    /// Returns a reference into PresetManager's internal strings;
    /// safe for use within a single thread of execution but do not
    /// store the reference across calls that might mutate the
    /// editing/controlled axes.
    [[nodiscard]] const std::string &current_apply_owner() noexcept;

} // namespace Transmog
