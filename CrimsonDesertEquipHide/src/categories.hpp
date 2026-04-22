#pragma once

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace EquipHide
{
    /**
     * @brief Equipment category identifiers.
     * @details Based on IndexedStringA hash ranges.
     *          See .idea/research/equip_hide_v2.md for full mapping.
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

    inline constexpr std::size_t CATEGORY_COUNT =
        static_cast<std::size_t>(Category::COUNT);

    constexpr std::string_view category_section(Category cat)
    {
        constexpr std::string_view names[] = {
            "OneHandWeapons", "TwoHandWeapons", "Shields", "Bows",
            "SpecialWeapons", "Tools", "Lanterns",
            "Helm", "Chest", "Legs", "Underwear", "Gloves", "Boots",
            "Cloak", "Shoulder", "Mask", "Glasses",
            "Earrings", "Rings", "Necklace", "Bags",
            "UserPreset1", "UserPreset2", "UserPreset3",
            "UserPreset4", "UserPreset5", "UserPreset6",
            "UserPreset7", "UserPreset8", "UserPreset9",
            "UserPreset10"};
        static_assert(std::size(names) == CATEGORY_COUNT,
                      "names[] must match Category enum");
        const auto idx = static_cast<std::size_t>(cat);
        if (idx >= CATEGORY_COUNT)
            return "Unknown";
        return names[idx];
    }

    constexpr uint8_t default_show_value([[maybe_unused]] Category cat)
    {
        return 0;
    }

    /** @brief Returns true if the category is a user-defined preset. */
    constexpr bool is_user_preset(Category cat)
    {
        return cat >= Category::UserPreset1 && cat <= Category::UserPreset10;
    }

    /**
     * @brief Returns true if the category is an armor type.
     * @details Armor requires map entry injection rather than just vis-byte
     *          modification. User presets may contain armor parts, so they
     *          are included.
     */
    constexpr bool is_armor_category(Category cat)
    {
        switch (cat)
        {
        case Category::Helm:
        case Category::Chest:
        case Category::Legs:
        case Category::Underwear:
        case Category::Gloves:
        case Category::Boots:
        case Category::Cloak:
        case Category::Shoulder:
        case Category::Mask:
        case Category::Glasses:
        case Category::Earrings:
        case Category::Rings:
        case Category::Necklace:
        case Category::Bags:
            return true;
        default:
            return is_user_preset(cat);
        }
    }

    // --- Per-category runtime state ---
    struct CategoryState
    {
        std::atomic<bool> enabled{true};
        std::atomic<bool> hidden{false};
        DMK::Config::KeyComboList toggleHotkeyCombos;
        DMK::Config::KeyComboList showHotkeyCombos;
        DMK::Config::KeyComboList hideHotkeyCombos;
    };

    std::array<CategoryState, CATEGORY_COUNT> &category_states();

    /** @brief Returns the comma-separated default Parts string for a category. */
    std::string default_parts_string(Category cat);

    // --- Runtime hash resolution ---

    /**
     * @brief Supply runtime-resolved name-to-hash mappings from the IndexedStringA table.
     * @details Must be called before load_config / register_parts for runtime
     *          resolution to take effect. If not called, compile-time fallback
     *          hashes are used.
     */
    void set_runtime_hashes(std::unordered_map<std::string, uint32_t> &&nameToHash);

    /** @brief Total number of known part definitions. */
    std::size_t total_part_count() noexcept;

    /**
     * @brief Returns names of known parts not present in the given map.
     * @details Used by the table scanner to track resolution progress.
     */
    std::vector<std::string> get_unresolved_parts(
        const std::unordered_map<std::string, uint32_t> &resolved);

    /**
     * @brief Returns the minimum hash value of the contiguous block.
     * @details Loaded with relaxed ordering; stale bounds only widen the range
     *          check, causing harmless extra map lookups.
     */
    uint32_t hash_range_min() noexcept;

    /** @brief Returns the maximum hash value of the contiguous block. */
    uint32_t hash_range_max() noexcept;

    /** @brief Returns true if the hash is a registered outlier (outside the contiguous range). */
    bool is_outlier_hash(uint32_t hash) noexcept;

    // --- Part classification ---

    /** @brief Parse a "Parts" string and register all contained IDs for the given category.
     *  @param storeBase If true (default), also caches partsStr as the base Parts for
     *                   subsequent rebuilds. Set false when applying a per-character
     *                   override so the base Parts string is preserved. */
    void register_parts(Category cat, const std::string &partsStr, bool storeBase = true);

    /** @brief Finalize the lookup map and compute hash range bounds. Call after all register_parts(). */
    void build_part_lookup();

    // --- Per-character Parts overrides ---

    /** @brief Number of supported protagonist identities for per-char overrides. */
    inline constexpr std::size_t kCharIdxCount = 3;

    /** @brief Returns human-readable name for a character index (0=Kliff, 1=Damiane, 2=Oongka).
     *  @return Non-empty string_view for valid indices, empty view otherwise. */
    std::string_view character_name_for_idx(std::size_t idx) noexcept;

    /** @brief Store a per-character Parts override. Empty = inherit from the base [Section].
     *  @note Does not rebuild; takes effect on the next set_active_character() or rebuild call. */
    void set_per_char_parts(Category cat, std::size_t charIdx, std::string partsStr);

    /** @brief Update the active character index. Triggers rebuild_part_lookup() on change.
     *  @param charIdx 0..kCharIdxCount-1 for Kliff/Damiane/Oongka, or -1 to use only base Parts. */
    void set_active_character(int charIdx);

    /** @brief Returns the currently active character index, or -1 if using base only. */
    int get_active_character() noexcept;

    /**
     * @brief Re-resolve part names against current runtime hashes and rebuild the lookup map.
     * @details Used when the deferred table scan completes after config has already
     *          been loaded with fallback hashes.
     */
    void rebuild_part_lookup();

    /** @brief Category bitmask type -- one bit per category (supports up to 32). */
    using CategoryMask = uint32_t;

    /** @brief Returns a bitmask with only the given category's bit set. */
    constexpr CategoryMask category_bit(Category cat)
    {
        return CategoryMask{1} << static_cast<uint8_t>(cat);
    }

    /**
     * @brief Classify a part hash into a bitmask of all categories it belongs to.
     * @return 0 if the hash is not tracked.
     */
    CategoryMask classify_part(uint32_t partHash);

    /**
     * @brief Fast pre-filter: returns true if a hash has any classification entry.
     * @details Uses a 64K-bit bitset (8 KB) covering the full 16-bit hash space.
     *          Single memory access replaces the range check + outlier scan.
     */
    bool needs_classification(uint32_t hash) noexcept;

    /** @brief Returns true if a specific category is currently hidden (and enabled). */
    bool is_category_hidden(Category cat);

    /** @brief Returns true if ANY category in the given bitmask is currently hidden. */
    bool is_any_category_hidden(CategoryMask mask);

    /** @brief Recompute cached hidden-state masks from category_states(). Call after any mutation. */
    void update_hidden_mask();

    /**
     * @brief Returns the registered part-hash to category-mask map by reference.
     * @details Double-buffered: readers see the active map while rebuild_part_lookup()
     *          writes to the inactive one and flips. Safe as long as iteration does
     *          not overlap a second rebuild.
     */
    const std::unordered_map<uint32_t, CategoryMask> &get_part_map();

} // namespace EquipHide
