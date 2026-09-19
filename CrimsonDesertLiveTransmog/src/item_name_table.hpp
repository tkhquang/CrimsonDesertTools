#ifndef TRANSMOG_ITEM_NAME_TABLE_HPP
#define TRANSMOG_ITEM_NAME_TABLE_HPP

#include "shared_state.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
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
     * refcounted string wrapper at descriptor+8. The game bakes these names into its data, and they survive a game
     * patch ORDERS OF MAGNITUDE more often than the raw uint16 item_id (which is a per-record descriptor index that
     * can shift on any content patch).
     *
     * `resolve_chain` in item_name_table.cpp walks SubTranslator to the iteminfo global pointer holder and to
     * ItemAccessor, through bounded AOB scans and guarded RIP-relative decodes. The chain hardcodes no absolute
     * address at any hop. The catalog walk then reads the entry count and the descriptor pointer array off the
     * container, and each descriptor yields one stable per-item name.
     *
     * Both container offsets live in item_name_table.cpp. Verify them against live memory on patch day. The builder
     * fully populates the table at startup.
     */
    class ItemNameTable
    {
    public:
        /// Returns the process-wide catalog instance.
        static ItemNameTable &instance();

        /**
         * @brief Return value for build(). It separates the retryable case (global pointer not yet initialized) from
         *        the fatal case (address chain broken or decoder mismatch).
         */
        enum class BuildResult
        {
            /// The walk succeeded and the table holds the catalog.
            Ok,
            /// The address chain resolved, but the iteminfo global is still null. Retry later on a background thread.
            Deferred,
            /**
             * @brief Address resolution failed (bounded AOB anchor missed, no relative call found). Do not retry,
             *        because the address chain is broken.
             */
            Fatal,
        };

        /**
         * @brief Build the table from the item descriptor catalog.
         *
         * @param sub_translator_addr Address of SubTranslator (the SlotPopulator item-id translator), resolved via AOB.
         * @return BuildResult describing the outcome. `Deferred` is retryable. `Ok` and `Fatal` are final.
         *
         * On `Ok`, `size()` reflects the ingested entry count. On `Deferred`/`Fatal`, the table stays empty. A
         * background scan thread can call it safely. `m_mutex` guards the published state, so `ready()` and
         * `sorted_entries()` observe a consistent snapshot.
         */
        [[nodiscard]] BuildResult build(uintptr_t sub_translator_addr);

        /**
         * @brief Look up an item name by id. Returns an empty string if the id is unknown.
         */
        [[nodiscard]] std::string name_of(uint16_t item_id) const;

        /**
         * @brief Resolve a previously-saved item name back to its current id. Returns std::nullopt on miss.
         */
        [[nodiscard]] std::optional<uint16_t> id_of(std::string_view name) const;

        /**
         * @brief True if the item's descriptor has a non-sentinel pointer at the variant-metadata slot (see
         *        `DESC_VARIANT_META_OFFSET` in item_name_table.cpp for the offset and for how to re-derive it).
         *
         * Items with this flag set are members of an engine-internal linked list threaded via that slot. Flagged items
         * do not render via runtime transmog on the player. The exact semantic meaning of the meta struct is not fully
         * mapped. Users see these as "damaged" in-game, but the catalog-wide population includes non-armor readables
         * too, so the label is intentionally mechanism-neutral. The overlay treats this as "may not render" and warns
         * in the picker.
         *
         * Detector: build() resolves the sentinel pointer STATISTICALLY, as the mode of
         * `*(desc+DESC_VARIANT_META_OFFSET)` across all valid descriptors. The clear majority of items share it. No
         * address is hardcoded, so the detector self-heals across future .data shuffles. Returns false for unknown ids
         * or when the catalog is not yet built.
         */
        [[nodiscard]] bool has_variant_meta(uint16_t item_id) const;

        /**
         * @brief True if the item is safe to equip on the (male) player.
         *
         * Kliff-centric: an item is player-safe unless the female body restricts it. `m_body_by_name` carries that
         * restriction, keyed by lowercase internal name. An item wearable by both bodies or unrestricted is absent
         * from that map and counts as safe.
         *
         * Unknown ids default to `true`: the picker prefers to surface an item rather than hide it accidentally.
         */
        [[nodiscard]] bool is_player_compatible(uint16_t item_id) const;

        /// Returns true once build() published a non-empty catalog.
        [[nodiscard]] bool ready() const noexcept { return !m_id_to_name.empty(); }

        /// Returns the number of catalog entries build() published.
        [[nodiscard]] std::size_t size() const noexcept { return m_id_to_name.size(); }

        /**
         * @brief Read the live descriptor pointer for an item.
         *
         * Dereferences the iteminfo global cached during build(), walks `*(global_ptr + ITEMINFO_PTR_ARRAY_OFFSET)`
         * (ptr_array) and returns `ptr_array[item_id*8]`. Returns 0 on any fault or if the catalog is not yet built.
         * Thread-safe. It reads only and never mutates.
         */
        [[nodiscard]] uintptr_t descriptor_of(uint16_t item_id) const noexcept;

        /**
         * @brief Live ptr_array base and entry count.
         *
         * Returns {ptr_array, count}. Either or both can be 0 if the catalog global is not yet initialized. Callers
         * MUST verify both > 0 before indexing.
         */
        struct CatalogInfo
        {
            uintptr_t ptr_array = 0;
            uint32_t count = 0;
        };
        [[nodiscard]] CatalogInfo catalog_info() const noexcept;

        /**
         * @brief Address of ItemAccessor (IndexedStringA short->hash lookup), cached during the build() chain walk.
         *
         * This function has many byte-identical template-instantiation siblings, so it cannot be AOB-located directly.
         * `resolve_chain` reaches it through the same bounded-AOB chain the catalog walk uses (SubTranslator anchor ->
         * item descriptor initializer -> first relative call). Returns 0 if no successful resolve_chain call landed
         * yet (either before the first build() or after a fatal decoder mismatch).
         *
         * Thread-safe after a successful build() or deferred-scan retry. The chain walk reads static exe bytes only
         * and does NOT depend on the iteminfo global, so this address becomes valid even on the first
         * BuildResult::Deferred return.
         */
        [[nodiscard]] uintptr_t indexed_string_lookup_addr() const noexcept;

        /**
         * @brief Address of the iteminfo global holder slot, cached during the same build() chain walk.
         *
         * This is the ONLY way to reach that slot. The engine's per-manager accessors are byte-identical template
         * clones that differ only in their RIP displacement, so a global AOB cut from one of them matches every
         * manager at once and can never be unique - and the slot itself has exactly one referencing instruction in
         * the whole image, which lives inside such a clone. The bounded chain walk sidesteps both problems by
         * reaching the right clone through the call graph first and only then reading its displacement.
         *
         * Returns 0 before the first successful resolve_chain call. Like indexed_string_lookup_addr(), it becomes
         * valid even on a BuildResult::Deferred return, because the walk reads static exe bytes only.
         */
        [[nodiscard]] uintptr_t iteminfo_holder_addr() const noexcept;

        /**
         * @brief Body-type classification that drives the picker's per-character visibility.
         *
         * An item rendered on the wrong body produces broken meshes, so the filter hides opposite-body items by
         * default.
         *
         * `m_body_by_name` is the live source. It lists only single-body restricted items, so classification resolves
         * to Male, Female, or Generic. The remaining kinds stay as picker display vocabulary for a future mesh-based
         * classifier.
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
            uint16_t id{};
            /// Transmog slot from the item's ItemGroupInfo membership, or Count for non-equipment.
            TransmogSlot category{TransmogSlot::Count};
            bool has_variant_meta{};
            bool is_player_compatible{};
            BodyKind body_kind{BodyKind::Generic};
            std::string name;
            /// Human-readable name from the display_names TSV.
            std::string display_name;
            /**
             * @brief Logical-order text for search matching, set only for a pre-shaped locale.
             *
             * @details A right-to-left locale stores `display_name` already shaped into presentation forms and
             *          already reordered, because Dear ImGui performs neither pass. That form can never match what an
             *          input method produces, so the pack carries the original text separately and the picker matches
             *          against both. Empty when `display_name` is itself searchable.
             */
            std::string search_name;
        };

        /**
         * @brief Flat, alphabetically-sorted entry list for UI iteration.
         *
         * @return A shared snapshot. The first access after build() builds it, and later calls return the same one
         *         until a load_display_names() call retires it.
         *
         * @details The return is a `shared_ptr` rather than a reference for one reason: `load_display_names()` can
         *          run on the deferred catalog worker while the picker iterates this list on the render thread. A
         *          holder keeps its own snapshot alive, so a retire on another thread cannot free the elements under
         *          it. Copying the pointer is one refcount bump per frame, not a copy of several thousand entries.
         *
         * @note This const method fills `m_sorted_cache` under `m_mutex`.
         */
        [[nodiscard]] std::shared_ptr<const std::vector<Entry>> sorted_entries() const;

        /**
         * @brief Map a character name to its body kind.
         * @return The body kind, or `BodyKind::Generic` for an unknown name, so a future character gets a wide-open
         *         picker instead of an empty one.
         */
        [[nodiscard]] static BodyKind body_kind_for_character(std::string_view char_name) noexcept;

        /**
         * @brief Single-body restriction for an item: BodyKind::Male / BodyKind::Female, or BodyKind::Generic when the
         *        item is dual-body / unrestricted (or the id is unknown / catalog not yet built).
         *
         * Unlike is_player_compatible (which is Kliff-centric), this returns the raw kind so callers can compare it
         * against a specific character's body - e.g. to decide whether the engine's own body/class check will accept
         * the item (and therefore pick the correct body variant) WITHOUT LT's char-class bypass. Sourced from the same
         * display_names equip-eligibility column (m_body_by_name, keyed by lowercase internal name).
         *
         * Named to parallel body_kind_for_character(). It is distinct from PresetManager::body_kind_of(), which
         * returns a character's configured body as a string.
         */
        [[nodiscard]] BodyKind body_kind_for_item(uint16_t item_id) const;

        /**
         * @brief Look up the transmog slot for an item id.
         *
         * Resolved at catalog-build time in two steps: the item's ItemGroupInfo membership matched by GROUP NAME,
         * then, for items whose groups name no slot (NPC and boss gear), a `type_code -> slot` table LEARNED from the
         * items the first step classified. See the slot-classification block and `DESC_TYPE_CODE_OFFSET` in
         * item_name_table.cpp. Nothing is hardcoded to a type-code value, because those are row indices that renumber
         * on patches.
         *
         * Anything not in a mapped group (shields aside, that means horse and pet gear, quest items, consumables) and
         * any unknown id returns `TransmogSlot::Count`.
         */
        [[nodiscard]] TransmogSlot category_of(uint16_t item_id) const noexcept;

        /**
         * @brief `category_of` WITHOUT the runtime-observed override - the catalog's own answer.
         *
         * Cross-checks the engine's live slot tags against `SLOT_METADATA`. `category_of` consults `m_observed_slot`
         * first, and the same auth-table walk that performs the check writes that map. A comparison against it is
         * self-fulfilling and never reports drift.
         */
        [[nodiscard]] TransmogSlot catalog_category_of(uint16_t item_id) const noexcept;

        /**
         * @brief Record an observed `(item_id -> slot)` binding seen in the engine's live auth-table.
         *
         * `category_of()` consults the runtime map BEFORE the catalog classification, so ground truth classifies an
         * item the engine actually equipped. Slot == `TransmogSlot::Count` clears the entry. Thread-safe and cheap:
         * one hash insert per call.
         */
        void record_observed_slot(std::uint16_t item_id, TransmogSlot slot) noexcept;

        /// Number of currently-recorded runtime slot observations. Diagnostic only.
        [[nodiscard]] std::size_t observed_slot_count() const noexcept;

        /**
         * @brief Dump the full catalog to a TSV file next to the game exe.
         * @note Columns: ItemID, Slot, Variant, PlayerSafe, Name.
         */
        void dump_catalog_tsv() const;

        /**
         * @brief Rebuild the display-name and wearer-body layers for one locale.
         *
         * @param locale_tag Archive locale tag such as "zho-cn". An empty view or "eng" loads the English baseline
         *        alone.
         * @param tsv_path Path to the shipped English display-names TSV. The two user override files derive their
         *        names from it. Build it from the wide runtime directory so a non-ASCII install path still resolves.
         *
         * @details Five layers apply in this order, each keyed by the lowercased item INTERNAL name:
         *          1. the pack "body" block, the only source of the wearer-body restriction,
         *          2. the pack "names" block for "eng", the display-name baseline,
         *          3. the pack "names" block for @p locale_tag, display names only, plus its "search" block when
         *             the locale is pre-shaped,
         *          4. `tsv_path` itself, ONLY when the pack supplied no names. It is a COMPLETE English table, so
         *             applying it over a locale would rewrite every name back to English,
         *          5. `<stem>.override.tsv` then `<stem>.<tag>.tsv`, the user overrides, which always apply.
         *
         *          An override overlays rather than replaces, so a partial file cannot blank the body column for
         *          the items it omits. With no pack at all, layers 4 and 5 carry the table on their own.
         *
         *          When no layer supplies a name, nothing publishes, so a locale switch that finds no source leaves
         *          the loaded table in place. Call it after a successful build(); calling it again switches locale.
         *          It retires the sorted cache, which a holder keeps reading, so the deferred catalog worker can
         *          call it while the picker draws.
         */
        void load_display_names(std::string_view locale_tag, const std::filesystem::path &tsv_path);

        /**
         * @brief Look up a display name by internal name.
         *
         * @param internal_name The item's internal catalog name.
         * @return The human-readable display name, or empty string if no mapping exists.
         */
        [[nodiscard]] std::string display_name_of(std::string_view internal_name) const;

    private:
        ItemNameTable() = default;

        /**
         * @brief Hash that accepts any string-like key, so a lookup by view finds a std::string entry.
         * @details std::unordered_map only allows a heterogeneous key when BOTH the hasher and the
         *          comparator declare is_transparent. That pair is what keeps id_of's string_view
         *          parameter from having to materialize a std::string on every call.
         */
        struct StringHash
        {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept
            {
                return std::hash<std::string_view>{}(key);
            }
        };

        std::unordered_map<uint16_t, std::string> m_id_to_name;
        std::unordered_map<std::string, uint16_t, StringHash, std::equal_to<>> m_name_to_id;
        std::unordered_map<uint16_t, uint8_t> m_variant_flag;
        // `item_id -> TransmogSlot`, classified during build() from the item's ItemGroupInfo membership. Items that
        // belong to no mapped group are ABSENT rather than stored as Count, so the map sizes to the equipment subset.
        std::unordered_map<uint16_t, TransmogSlot> m_slot_by_id;
        // Runtime-learned `item_id -> TransmogSlot` map. Populated by `record_observed_slot` (called from the
        // slot-discovery dump when it observes live auth-table bindings). Authoritative override for the catalog
        // classification: if the engine actually equipped an item in a given slot, that beats any derivation.
        // Session-scoped, with no disk persistence.
        std::unordered_map<uint16_t, TransmogSlot> m_observed_slot;
        std::unordered_map<std::string, std::string> m_display_names; // lowercase internal -> display
        // Lowercase internal -> logical-order search text. Populated only for a pre-shaped locale, where
        // m_display_names holds presentation forms that no typed query can match. Empty for every other locale.
        std::unordered_map<std::string, std::string> m_search_names;
        // Wearer-body restriction, and the ONLY source of one. It loads from the optional 3rd column of the
        // display_names TSV, keyed by lowercase internal name. Only single-body-restricted items are present (Male /
        // Female). Absent -> unrestricted (BodyKind::Generic). It drives the per-character picker filter and
        // is_player_compatible. The descriptor rule-classifier token walk carries no usable body class, because a
        // game update re-keyed those tokens. Do not go back to it.
        std::unordered_map<std::string, BodyKind> m_body_by_name;
        // Null until the first sorted_entries() call builds it, and retired by every load_display_names(). Held by
        // shared_ptr so a retire cannot free elements a caller is still reading. See sorted_entries().
        mutable std::shared_ptr<const std::vector<Entry>> m_sorted_cache;

        // Stability detector: it tracks the valid count from the previous build() attempt. The catalog is accepted
        // only when two consecutive scans produce the same count. An unchanged count means the game finished the
        // descriptor pointer array.
        uint32_t m_last_build_valid = 0;

        // The one lock this class takes, so there is no lock order to preserve. It serializes the build() publish
        // against every reader: name_of, id_of, has_variant_meta, is_player_compatible, body_kind_for_item,
        // category_of, catalog_category_of, record_observed_slot, observed_slot_count, sorted_entries and
        // load_display_names. It is mutable, because the const sorted_entries() fills m_sorted_cache under it.
        mutable std::mutex m_mutex;
    };

} // namespace Transmog

#endif // TRANSMOG_ITEM_NAME_TABLE_HPP
