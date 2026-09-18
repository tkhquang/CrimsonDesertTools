#ifndef EQUIPHIDE_CATEGORIES_HPP
#define EQUIPHIDE_CATEGORIES_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace EquipHide
{
    /**
     * @brief Equipment category identifiers.
     * @details Based on IndexedStringA hash ranges. See .idea/research/equip_hide_v2.md for full mapping.
     */
    enum class Category : uint8_t
    {
        OneHandWeapons,
        TwoHandWeapons,
        Shields,
        Bows,
        SpecialWeapons,
        Tools,
        Lanterns,
        Helm,
        Chest,
        Legs,
        Underwear,
        Gloves,
        Boots,
        Cloak,
        Shoulder,
        Mask,
        Glasses,
        Earrings,
        Rings,
        Necklace,
        Bags,
        UserPreset1,
        UserPreset2,
        UserPreset3,
        UserPreset4,
        UserPreset5,
        UserPreset6,
        UserPreset7,
        UserPreset8,
        UserPreset9,
        UserPreset10,
        COUNT
    };

    inline constexpr std::size_t CATEGORY_COUNT = static_cast<std::size_t>(Category::COUNT);

    /** @brief Returns the INI section name for a category, or "Unknown" for an out-of-range value. */
    [[nodiscard]] constexpr std::string_view category_section(Category cat) noexcept
    {
        constexpr std::string_view names[] = {"OneHandWeapons", "TwoHandWeapons", "Shields",     "Bows",
                                              "SpecialWeapons", "Tools",          "Lanterns",    "Helm",
                                              "Chest",          "Legs",           "Underwear",   "Gloves",
                                              "Boots",          "Cloak",          "Shoulder",    "Mask",
                                              "Glasses",        "Earrings",       "Rings",       "Necklace",
                                              "Bags",           "UserPreset1",    "UserPreset2", "UserPreset3",
                                              "UserPreset4",    "UserPreset5",    "UserPreset6", "UserPreset7",
                                              "UserPreset8",    "UserPreset9",    "UserPreset10"};
        static_assert(std::size(names) == CATEGORY_COUNT, "names[] must match Category enum");
        const auto idx = static_cast<std::size_t>(cat);
        if (idx >= CATEGORY_COUNT)
            return "Unknown";
        return names[idx];
    }

    /** @brief Returns true if the category is a user-defined preset. */
    [[nodiscard]] constexpr bool is_user_preset(Category cat) noexcept
    {
        return cat >= Category::UserPreset1 && cat <= Category::UserPreset10;
    }

    /** @brief Per-category runtime state: the INI enable flag and the live hide flag. */
    struct CategoryState
    {
        std::atomic<bool> enabled{true};
        std::atomic<bool> hidden{false};
    };

    /** @brief Returns the per-category state array by reference. */
    [[nodiscard]] std::array<CategoryState, CATEGORY_COUNT> &category_states() noexcept;

    /** @brief Returns the comma-separated default Parts string for a category. */
    [[nodiscard]] std::string default_parts_string(Category cat);

    // Runtime hash resolution

    /**
     * @brief Supply runtime-resolved name-to-hash mappings from the IndexedStringA table.
     * @details Must be called before load_config / register_parts for runtime resolution to take effect. If not called,
     *          compile-time fallback hashes are used.
     */
    void set_runtime_hashes(std::unordered_map<std::string, uint32_t> &&name_to_hash);

    /** @brief Total number of known part definitions. */
    [[nodiscard]] std::size_t total_part_count() noexcept;

    /**
     * @brief Returns names of known parts not present in the given map.
     * @details Used by the table scanner to track resolution progress.
     */
    [[nodiscard]] std::vector<std::string>
    get_unresolved_parts(const std::unordered_map<std::string, uint32_t> &resolved);

    // Part classification

    /**
     * @brief Parse a "Parts" string and register all contained IDs for the given category.
     * @param store_base If true (default), also caches parts_str as the base Parts for subsequent rebuilds. Set false
     *                   when applying a per-character override so the base Parts string is preserved.
     */
    void register_parts(Category cat, const std::string &parts_str, bool store_base = true);

    /** @brief Finalize the lookup map and compute hash range bounds. Call after all register_parts(). */
    void build_part_lookup();

    // Per-character Parts overrides

    /** @brief Number of supported protagonist identities for per-char overrides. */
    inline constexpr std::size_t k_charIdxCount = 3;

    /**
     * @brief Returns the human-readable name for a character index (0=Kliff, 1=Damiane, 2=Oongka).
     * @return Non-empty string_view for valid indices, empty view otherwise.
     */
    [[nodiscard]] std::string_view character_name_for_idx(std::size_t idx) noexcept;

    /**
     * @brief Store a per-character Parts override. Empty = inherit from the base [Section].
     * @note Does not rebuild. The override takes effect on the next set_active_character() or rebuild call.
     */
    void set_per_char_parts(Category cat, std::size_t char_idx, std::string parts_str);

    /**
     * @brief Update the active character index. Triggers rebuild_part_lookup() on change.
     * @param char_idx 0..k_charIdxCount-1 for Kliff/Damiane/Oongka, or -1 to use only base Parts.
     */
    void set_active_character(int char_idx);

    /**
     * @brief Re-resolve part names against current runtime hashes and rebuild the lookup map.
     * @details Used when the deferred table scan completes after config has already been loaded with fallback hashes.
     */
    void rebuild_part_lookup();

    /** @brief Category bitmask type - one bit per category (supports up to 32). */
    using CategoryMask = uint32_t;

    /** @brief Returns a bitmask with only the given category's bit set. */
    [[nodiscard]] constexpr CategoryMask category_bit(Category cat) noexcept
    {
        return CategoryMask{1} << static_cast<uint8_t>(cat);
    }

    /**
     * @brief Classify a part hash into a bitmask of all categories it belongs to.
     * @return 0 if the hash is not tracked.
     */
    [[nodiscard]] CategoryMask classify_part(uint32_t part_hash) noexcept;

    /**
     * @brief Fast pre-filter: returns true if a hash has any classification entry.
     * @details Uses a 64K-bit bitset (8 KB) covering the full 16-bit hash space. Single memory access replaces the
     *          range check + outlier scan.
     */
    [[nodiscard]] bool needs_classification(uint32_t hash) noexcept;

    /** @brief Returns true if a specific category is currently hidden (and enabled). */
    [[nodiscard]] bool is_category_hidden(Category cat) noexcept;

    /** @brief Returns true if ANY category in the given bitmask is currently hidden. */
    [[nodiscard]] bool is_any_category_hidden(CategoryMask mask) noexcept;

    /**
     * @brief Returns true if ANY category in the bitmask is hidden FOR a specific protagonist idx.
     * @details Looks up a per-character classification map keyed on char_idx (0=Kliff, 1=Damiane, 2=Oongka). Each
     *          per-character map merges the base [Section] Parts with that character's [Section:CharName] Parts
     *          overrides exactly as the active-character map does, but every map is kept resident simultaneously so
     *          per-vis-ctrl writes can resolve the right hide mask without re-running set_active_character.
     *
     *          char_idx == -1 (unknown body / NPC follower / pre-resolve tick) falls back to the active-character map
     *          so a slot with unresolved identity mirrors the active character's hide state (single-character
     *          semantics).
     * @param mask Category bitmask, the part-classification value returned by classify_part.
     * @param char_idx 0..k_charIdxCount-1 for a known protagonist, or -1 for the active-character fallback.
     */
    [[nodiscard]] bool is_any_category_hidden_for(CategoryMask mask, int char_idx) noexcept;

    /**
     * @brief Classify a part hash USING a specific character's part map.
     * @details Mirror of classify_part keyed on char_idx. char_idx == -1 falls back to the active-character map so an
     *          unidentified slot still classifies correctly under single-character semantics.
     * @return 0 if the hash is not tracked in the requested character's (or the active character's, on -1) part map.
     */
    [[nodiscard]] CategoryMask classify_part_for(uint32_t part_hash, int char_idx) noexcept;

    /** @brief Recompute cached hidden-state masks from category_states(). Call after any mutation. */
    void update_hidden_mask();

    /**
     * @brief Returns the registered part-hash to category-mask map by reference.
     * @details Double-buffered, so readers see the active map while rebuild_part_lookup() writes to the inactive one
     *          and flips. Safe as long as iteration does not overlap a second rebuild.
     */
    [[nodiscard]] const std::unordered_map<uint32_t, CategoryMask> &get_part_map() noexcept;

    /**
     * @brief Returns the per-character part map for a specific protagonist idx.
     * @details Built alongside the active-map double-buffer in rebuild_part_lookup(). Each character's map is a
     *          snapshot of (base [Section] Parts merged with that character's [Section:CharName] override) at the last
     *          rebuild point. The caller MUST pass a valid char_idx (0..k_charIdxCount-1). The active-character
     *          fallback is the consumer's responsibility (see is_any_category_hidden_for and the direct-write loop for
     *          the canonical pattern).
     */
    [[nodiscard]] const std::unordered_map<uint32_t, CategoryMask> &get_part_map_for(int char_idx) noexcept;

} // namespace EquipHide

#endif // EQUIPHIDE_CATEGORIES_HPP
