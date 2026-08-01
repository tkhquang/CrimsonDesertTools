#pragma once

#include "shared_state.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Transmog
{
    /**
     * @brief Stable string<->id catalog built from the game's item descriptor table (iteminfo).
     *
     * Each item has an internal name string (e.g. "Kliff_PlateArmor_Helm", "Marni_Laser_Helm_Upgrade") stored in a
     * refcounted string wrapper at descriptor+8. These names are baked game data and are ORDERS OF MAGNITUDE more
     * stable across game patches than the raw uint16 itemId (which is a per-record descriptor index that can shift on
     * any content patch).
     *
     * Resolution chain. No absolute address is hardcoded at any hop:
     *
     *   1. AOB-scan the SlotPopulator item-id translator (SubTranslator -- unique 1-hit).
     *   2. Bounded AOB scan of the first 0x80 bytes of SubTranslator for a unique prologue anchor preceding
     *      `E8 disp32` -> the item descriptor initializer. The call offset is NOT hardcoded. Two anchor encodings
     *      are tried: the 15-byte rsp-relative-lea form first, the 14-byte rbp-relative-lea form as fallback.
     *   3. Decode the first `E8` call inside the descriptor initializer -> the iteminfo descriptor accessor
     *      (ItemAccessor).
     *   4. Bounded AOB scan of the first 0x40 bytes of ItemAccessor for a local anchor preceding `48 8B 1D disp32`
     *      (mov rbx, [rip+disp]) -> the iteminfo global pointer holder.
     *   5. `*(holder)` = globalPtr (the item container).
     *   6. `*(globalPtr + k_iteminfoCountOffset)` = entry count (dword).
     *   7. `*(globalPtr + k_iteminfoPtrArrayOffset)` = descriptor pointer array base.
     *   8. For each itemId in [0, count):
     *        descriptor = *(ptrArray + itemId*8)
     *        wrapper    = *(descriptor + 8)
     *        nameCStr   = *(wrapper + 0)           // lowercased C string
     *      yields a stable, unique per-item name.
     *
     * Both container offsets live in item_name_table.cpp. Verify them against live memory on patch day. The table is
     * fully populated at startup.
     */
    class ItemNameTable
    {
    public:
        static ItemNameTable &instance();

        /**
         * @brief Return value for build(). It separates the retryable case (global pointer not yet initialized) from
         *        the fatal case (address chain broken or decoder mismatch).
         */
        enum class BuildResult
        {
            Ok,       // Catalog walked successfully, table populated.
            Deferred, // Address chain resolved, but the iteminfo global is still null. Retry later on a background
                      // thread.
            Fatal,    // Address resolution failed (bounded AOB anchor missed, no relative call found, etc). Do not
                      // retry, because the address chain is broken.
        };

        /**
         * @brief Build the table from the item descriptor catalog.
         *
         * @param subTranslatorAddr Address of SubTranslator (the SlotPopulator item-id translator), resolved via AOB.
         * @return BuildResult describing the outcome. `Deferred` is retryable. `Ok` and `Fatal` are final.
         *
         * On `Ok`, `size()` reflects the ingested entry count. On `Deferred`/`Fatal`, the table stays empty. It is
         * thread-safe to call from a background scan thread. Internal state is guarded, so `ready()` and
         * `sorted_entries()` observe a consistent snapshot.
         */
        BuildResult build(uintptr_t subTranslatorAddr);

        /**
         * @brief Look up an item name by id. Returns an empty string if the id is unknown.
         */
        std::string name_of(uint16_t itemId) const;

        /**
         * @brief Resolve a previously-saved item name back to its current id. Returns std::nullopt on miss.
         */
        std::optional<uint16_t> id_of(const std::string &name) const;

        /**
         * @brief True if the item's descriptor has a non-sentinel pointer at the variant-metadata slot (see
         *        `k_descVariantMetaOffset` in item_name_table.cpp for the offset and for how to re-derive it).
         *
         * Items with this flag set are members of an engine-internal linked list threaded via that slot. Flagged items
         * do not render via runtime transmog on the player. The exact semantic meaning of the meta struct is not fully
         * mapped. Users see these as "damaged" in-game, but the catalog-wide population includes non-armor readables
         * too, so the label is intentionally mechanism-neutral. The overlay treats this as "may not render" and warns
         * in the picker.
         *
         * Detector: the sentinel pointer is resolved STATISTICALLY at build() time as the mode of
         * `*(desc+k_descVariantMetaOffset)` across all valid descriptors. The clear majority of items share it. No
         * address is hardcoded, so the detector self-heals across future .data shuffles. Returns false for unknown ids
         * or when the catalog is not yet built.
         */
        bool has_variant_meta(uint16_t itemId) const;

        /**
         * @brief True if the item is safe to equip on the (male) player.
         *
         * Kliff-centric: an item is player-safe unless it is restricted to the female body. The restriction is sourced
         * from the equip-eligibility ("Male"/"Female") column of the display_names TSV (m_bodyByName, keyed by
         * lowercase internal name). An item wearable by both bodies or unrestricted is absent from that map and counts
         * as safe. The descriptor rule-classifier token walk is NOT a usable source for this, because a game update
         * re-keyed those tokens.
         *
         * Unknown ids default to `true`: the picker prefers to surface an item rather than hide it accidentally.
         */
        bool is_player_compatible(uint16_t itemId) const;

        bool ready() const noexcept { return !m_idToName.empty(); }
        std::size_t size() const noexcept { return m_idToName.size(); }

        /**
         * @brief Read the live descriptor pointer for an item.
         *
         * Dereferences the iteminfo global cached during build(), walks `*(globalPtr + k_iteminfoPtrArrayOffset)`
         * (ptrArray) and returns `ptrArray[itemId*8]`. Returns 0 on any fault or if the catalog is not yet built.
         * Thread-safe. It reads only and never mutates.
         */
        uintptr_t descriptor_of(uint16_t itemId) const noexcept;

        /**
         * @brief Live ptrArray base and entry count.
         *
         * Returns {ptrArray, count}. Either or both can be 0 if the catalog global is not yet initialized. Callers MUST
         * verify both > 0 before indexing.
         */
        struct CatalogInfo
        {
            uintptr_t ptrArray = 0;
            uint32_t count = 0;
        };
        CatalogInfo catalog_info() const noexcept;

        /**
         * @brief Address of ItemAccessor (IndexedStringA short->hash lookup), cached during the build() chain walk.
         *
         * This function has many byte-identical template-instantiation siblings, so it cannot be AOB-located directly.
         * Instead it is reached via the same bounded-AOB chain the catalog walk uses (SubTranslator anchor -> item
         * descriptor initializer -> first relative call). Returns 0 if no successful resolve_chain call landed yet
         * (either before the first build() or after a fatal decoder mismatch).
         *
         * Thread-safe after a successful build() or deferred-scan retry. The chain walk only reads static exe bytes and
         * does NOT depend on the iteminfo global being initialized, so this address becomes valid even on the first
         * BuildResult::Deferred return.
         */
        uintptr_t indexed_string_lookup_addr() const noexcept;

        /**
         * @brief Flat, alphabetically-sorted entry list for UI iteration.
         *
         * Rebuilt lazily on first access after build(). It is stable thereafter. Returned by const reference so the
         * overlay can hold onto it without copying the several thousand entries per frame.
         *
         * `category` is the transmog slot derived from the canonical item-type code at
         * `desc+k_descTypeCodeOffset` (see `category_of` below), or `TransmogSlot::Count` if the item is not a
         * transmog-eligible armor slot (weapon, shield, horse armor, quest item, etc).
         */
        /**
         * Body-type classification that drives the picker's per-character visibility: an item rendered on the wrong
         * body produces broken meshes, so the filter hides opposite-body items by default.
         *
         * The live source is the equip-eligibility column of the display_names TSV (m_bodyByName). Only single-body
         * restricted items are listed, so classification resolves to Male, Female, or Generic. The descriptor
         * rule-classifier token walk is NOT a usable source, because a game update re-keyed those tokens. The
         * remaining kinds stay as picker display vocabulary for a future mesh-based classifier.
         *
         *   Generic:     unrestricted / wearable by both bodies (also the default for unknown ids)
         *   Male:        restricted to the male humanoid skeleton
         *   Female:      restricted to the female humanoid skeleton
         *   Both:        wearable by both bodies (reserved, not currently emitted)
         *   Ambiguous:   humanoid item whose body cannot be decided. The picker shows an amber badge (reserved)
         *   NonHumanoid: mount/pet/wagon/dragon gear, hidden from all human-character pickers (reserved)
         */
        enum class BodyKind : std::uint8_t
        {
            Generic = 0,
            Male = 1,
            Female = 2,
            Both = 3,
            NonHumanoid = 4,
            Ambiguous = 5,
        };

        struct Entry
        {
            uint16_t id;
            TransmogSlot category;
            bool hasVariantMeta;
            bool isPlayerCompatible;
            BodyKind bodyKind;
            std::string name;
            std::string displayName; // human-readable name from the display_names TSV
        };
        const std::vector<Entry> &sorted_entries() const;

        /**
         * Map character name -> body kind. Returns `BodyKind::Generic` for unknown names, so future characters produce
         * a wide-open picker instead of an empty one.
         */
        static BodyKind body_kind_for_character(const std::string &charName) noexcept;

        /**
         * @brief Single-body restriction for an item: BodyKind::Male / BodyKind::Female, or BodyKind::Generic when the
         *        item is dual-body / unrestricted (or the id is unknown / catalog not yet built).
         *
         * Unlike is_player_compatible (which is Kliff-centric), this returns the raw kind so callers can compare it
         * against a specific character's body -- e.g. to decide whether the engine's own body/class check will accept
         * the item (and therefore pick the correct body variant) WITHOUT LT's char-class bypass. Sourced from the same
         * display_names equip-eligibility column (m_bodyByName, keyed by lowercase internal name).
         *
         * Named to parallel body_kind_for_character(). It is distinct from PresetManager::body_kind_of(), which
         * returns a character's configured body as a string.
         */
        BodyKind body_kind_for_item(uint16_t itemId) const;

        /**
         * @brief Look up the transmog slot for an item id.
         *
         * Driven by the canonical item-type code captured at `desc+k_descTypeCodeOffset` during the catalog build.
         * That field is the game-side classifier. See `slot_from_type_code` for the authoritative typeCode->slot
         * table.
         *
         * Every code that does not map to a transmog-eligible armor slot (shields, horse armor, quest items, ...) or an
         * unknown id returns `TransmogSlot::Count`.
         */
        TransmogSlot category_of(uint16_t itemId) const noexcept;

        /**
         * Raw item-type code u16 at `desc+k_descTypeCodeOffset` for the given item, or 0xFFFF if unknown. Useful for
         * slot-discovery research. Print it next to the live engine slot tag to reveal the (typeCode -> slot) mapping
         * that drives `slot_from_type_code`, so accessory and weapon codes can be added without static guessing.
         */
        std::uint16_t type_code_of(std::uint16_t itemId) const noexcept;

        /**
         * Record an observed `(itemId -> slot)` binding seen in the engine's live auth-table. `category_of()` consults
         * the runtime map BEFORE the static type-code map, so any item the engine actually equipped becomes correctly
         * categorized for the picker even when its type code is not yet listed in the static switch. Slot ==
         * `TransmogSlot::Count` clears the entry. Thread-safe and cheap: one hash insert per call.
         */
        void record_observed_slot(std::uint16_t itemId, TransmogSlot slot) noexcept;

        /**
         * Number of currently-recorded runtime slot observations. Diagnostic only.
         */
        std::size_t observed_slot_count() const noexcept;

        /**
         * Dump the full catalog to a TSV file next to the game exe.
         * Columns: ItemID, Slot, Variant, PlayerSafe, Name.
         */
        void dump_catalog_tsv() const;

        /**
         * @brief Load human-readable display names from a TSV file.
         *
         * Each line is `internalName<TAB>displayName`. Keys are lowercased at load time for case-insensitive matching.
         * Must be called after a successful build(). Invalidates the sorted cache so the next sorted_entries() picks up
         * display names.
         *
         * @param tsvPath Path to the display names TSV file.
         */
        void load_display_names(const std::string &tsvPath);

        /**
         * @brief Look up a display name by internal name.
         *
         * @param internalName The item's internal catalog name.
         * @return The human-readable display name, or empty string if no mapping exists.
         */
        [[nodiscard]] std::string display_name_of(std::string_view internalName) const;

    private:
        ItemNameTable() = default;

        std::unordered_map<uint16_t, std::string> m_idToName;
        std::unordered_map<std::string, uint16_t> m_nameToId;
        std::unordered_map<uint16_t, uint8_t> m_variantFlag;
        std::unordered_map<uint16_t, uint16_t> m_typeCode; // canonical item-type code from the descriptor
        // Runtime-learned `itemId -> TransmogSlot` map. Populated by `record_observed_slot` (called from the
        // slot-discovery dump when it observes live auth-table bindings). Authoritative override for the static
        // type-code switch: if the engine actually equipped an item in a given slot, that beats any heuristic.
        // Session-scoped, with no disk persistence.
        std::unordered_map<uint16_t, TransmogSlot> m_observedSlot;
        std::unordered_map<std::string, std::string> m_displayNames; // lowercase internal -> display
        // Wearer-body restriction, loaded from the optional 3rd column of the display_names TSV. Keyed by lowercase
        // internal name. Only single-body-restricted items are present (Male / Female). Absent -> unrestricted
        // (BodyKind::Generic). Drives the per-character picker filter and is_player_compatible. This replaces the
        // runtime rule-classifier tokens, which a game update re-keyed.
        std::unordered_map<std::string, BodyKind> m_bodyByName;
        mutable std::vector<Entry> m_sortedCache;

        // Stability detector: tracks the valid count from the previous build() attempt. The catalog is accepted only
        // when two consecutive scans produce the same count. An unchanged count means the game finished populating the
        // descriptor pointer array.
        uint32_t m_lastBuildValid = 0;
    };

} // namespace Transmog
