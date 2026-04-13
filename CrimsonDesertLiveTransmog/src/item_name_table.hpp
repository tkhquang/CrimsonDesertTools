#pragma once

#include "shared_state.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Transmog
{
    /**
     * @brief Stable string<->id catalog built from the game's item descriptor
     *        table (`qword_145CEF370` / iteminfo).
     *
     * Each item has an internal name string (e.g. "Kliff_PlateArmor_Helm",
     * "Marni_Laser_Helm_Upgrade") stored in a refcounted string wrapper at
     * descriptor+8. These names are baked game data and are ORDERS OF
     * MAGNITUDE more stable across game patches than the raw uint16
     * itemId (which is a per-record descriptor index that can shift on any
     * content patch).
     *
     * Resolution chain (verified against v1.02.00 via IDA + CE):
     *
     *   1. AOB-scan `sub_14076D950` (SlotPopulator translator — unique 1-hit).
     *   2. Bounded AOB scan of the first 0x80 bytes of sub_14076D950 for a
     *      unique 14-byte anchor preceding `E8 disp32` -> `sub_141D45270`
     *      (item descriptor initializer). Offset NOT hardcoded.
     *   3. Decode first `E8` call inside `sub_141D45270` ->
     *      `sub_1402D75D0` (iteminfo descriptor accessor).
     *   4. Bounded AOB scan of the first 0x40 bytes of sub_1402D75D0 for a
     *      local anchor preceding `48 8B 1D disp32` (mov rbx, [rip+disp]) ->
     *      `qword_145CEF370` = iteminfo global pointer holder.
     *   5. `*(qword_145CEF370)` = globalPtr (6024-entry item container).
     *   6. `*(globalPtr + 8)` = entry count (dword).
     *   7. `*(globalPtr + 80)` = descriptor pointer array base.
     *   8. For each itemId in [0, count):
     *        descriptor = *(ptrArray + itemId*8)
     *        wrapper    = *(descriptor + 8)
     *        nameCStr   = *(wrapper + 0)           // lowercased C string
     *      yields a stable, unique per-item name.
     *
     * The table is fully populated at startup (6024/6024 valid on v1.02.00).
     */
    class ItemNameTable
    {
    public:
        static ItemNameTable &instance();

        /**
         * @brief Return value for build(), distinguishing retryable
         *        (global pointer not yet initialized) from fatal
         *        (address chain broken or decoder mismatch).
         */
        enum class BuildResult
        {
            Ok,             // Catalog walked successfully, table populated.
            Deferred,       // Address chain resolved but the iteminfo
                            // global is still null — retry later on a
                            // background thread.
            Fatal,          // Address resolution failed (bounded AOB
                            // anchor missed, no relative call found, etc).
                            // Do not retry — address chain is broken.
        };

        /**
         * @brief Build the table from the item descriptor catalog.
         *
         * @param subTranslatorAddr Address of `sub_14076D950` (the
         *        SlotPopulator item-id translator), resolved via AOB.
         * @return BuildResult describing the outcome. Retryable when
         *         `Deferred`; final on `Ok` or `Fatal`.
         *
         * On `Ok`, `size()` reflects the ingested entry count. On
         * `Deferred`/`Fatal`, the table is left empty. Thread-safe to
         * call from a background scan thread; internal state is
         * guarded so `ready()` and `sorted_entries()` observe a
         * consistent snapshot.
         */
        BuildResult build(uintptr_t subTranslatorAddr);

        /**
         * @brief Look up an item name by id.
         *        Returns an empty string if the id is unknown.
         */
        std::string name_of(uint16_t itemId) const;

        /**
         * @brief Resolve a previously-saved item name back to its current id.
         *        Returns std::nullopt on miss.
         */
        std::optional<uint16_t> id_of(const std::string &name) const;

        /**
         * @brief True if the item's descriptor has a non-sentinel
         *        pointer at `+0x3A0` (i.e. a variant metadata struct).
         *
         * Items with this flag set are members of an engine-internal
         * linked list threaded via `desc+0x3A0`. Across the 14 labeled
         * armor samples we tested live, flagged items all failed to
         * render via runtime transmog on the player. The exact semantic
         * meaning of the meta struct is not fully mapped — users see
         * these as "damaged" in-game but the catalog-wide population
         * (~2399/6024 items) includes non-armor readables too, so the
         * label is intentionally mechanism-neutral. The overlay treats
         * this as "may not render" and warns in the picker.
         *
         * Detector: the sentinel pointer is resolved STATISTICALLY at
         * build() time as the mode of `*(desc+0x3A0)` across all valid
         * descriptors (~60% share it on v1.02.00). No hardcoded RVA —
         * the detector self-heals across future .data shuffles. Returns
         * false for unknown ids or when the catalog isn't yet built.
         */
        bool has_variant_meta(uint16_t itemId) const;

        /**
         * @brief True if the item is safe to equip on the player.
         *
         * An item is player-compatible if at least one of its rules in
         * the descriptor rule list at `+0x248` has a classifier-hash
         * array containing a known "player body-type" token. Items whose
         * rules use only non-player classifiers (horse/mount/pet tack,
         * wagon gear) are flagged unsafe — equipping them on the player
         * crashes the mesh binder downstream.
         *
         * Unknown ids and items with no rules default to `true`: the
         * picker prefers to surface inert cosmetics rather than hide
         * them accidentally. See `k_playerClassifiers` in the .cpp for
         * the specific token set and how to regenerate it for a new
         * game patch.
         */
        bool is_player_compatible(uint16_t itemId) const;

        bool ready() const noexcept { return !m_idToName.empty(); }
        std::size_t size() const noexcept { return m_idToName.size(); }

        /**
         * @brief Address of sub_1402D75D0 (IndexedStringA short->hash
         *        lookup), cached during the build() chain walk.
         *
         * This function has 50+ byte-identical template-instantiation
         * siblings in v1.02.00, so it cannot be AOB-located directly.
         * Instead it is reached via the same bounded-AOB chain the
         * catalog walk uses (subTranslator anchor -> sub_141D45270
         * -> first relative call). Returns 0 if no
         * successful resolve_chain call has landed yet (either before
         * the first build() or after a fatal decoder mismatch).
         *
         * Thread-safe after a successful build() or deferred-scan
         * retry. The chain walk only reads static exe bytes and does
         * NOT depend on the iteminfo global being initialized, so this
         * address becomes valid even on the first BuildResult::Deferred
         * return.
         */
        uintptr_t indexed_string_lookup_addr() const noexcept;

        /**
         * @brief Flat, alphabetically-sorted entry list for UI iteration.
         *
         * Rebuilt lazily on first access after build(); stable thereafter.
         * Returned by const reference so the overlay can hold onto it
         * without copying the 6000+ entries per frame.
         *
         * `category` is the auto-detected transmog slot (see
         * classify_slot below), or `TransmogSlot::Count` if the item
         * doesn't match any known armor-slot suffix (weapon, consumable,
         * recipe, quest item, etc).
         */
        struct Entry
        {
            uint16_t id;
            TransmogSlot category;
            bool hasVariantMeta;
            bool isPlayerCompatible;
            std::string name;
        };
        const std::vector<Entry> &sorted_entries() const;

        /**
         * @brief Classify an item name into a transmog slot by suffix.
         *
         * Rejects known non-equipment prefixes (`CraftingRecipe_`,
         * `Recipe_`, `Book_`, `Item_Skill_`) first — these items
         * sometimes carry armor-slot suffixes but are recipes/books,
         * not equippable armor (CE-verified: all have ruleCount=0).
         *
         * Then strips trailing noise (`_Upgrade\\d*`, roman numerals,
         * `_\\d+`, size tags) and case-insensitively matches the final
         * underscore-delimited token:
         *
         *   `_Helm`                 -> Helm
         *   `_Armor`, `_Cloth`      -> Chest
         *   `_Cloak`                -> Cloak
         *   `_Gloves`               -> Gloves
         *   `_Boots`                -> Boots
         *
         * Any other tail returns `TransmogSlot::Count` (unclassified).
         * CE-verified against the full 6024-entry v1.02.00 catalog:
         * Helm=367, Armor=984, Cloth=7, Cloak=111, Gloves=352, Boots=339.
         * (Includes 4 lowercase `_cloak` items and 12 recipe false
         * positives now correctly excluded.)
         */
        static TransmogSlot classify_slot(const std::string &name) noexcept;

        /// Dump the full catalog to a TSV file next to the game exe.
        /// Columns: ItemID, Slot, Variant, PlayerSafe, Name.
        void dump_catalog_tsv() const;

    private:
        ItemNameTable() = default;

        std::unordered_map<uint16_t, std::string> m_idToName;
        std::unordered_map<std::string, uint16_t> m_nameToId;
        std::unordered_map<uint16_t, uint8_t> m_variantFlag;
        std::unordered_map<uint16_t, uint8_t> m_playerFlag;
        mutable std::vector<Entry> m_sortedCache;

        // Stability detector: tracks the valid count from the previous
        // build() attempt.  The catalog is accepted only when two
        // consecutive scans produce the same count, meaning the game
        // has finished populating the descriptor pointer array.
        uint32_t m_lastBuildValid = 0;
    };

} // namespace Transmog
