#pragma once

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace EquipHide
{
    // =========================================================================
    // Category enumeration
    //
    // Based on IndexedStringA hash ranges verified via CE runtime analysis.
    // See .idea/research/equip_hide_v2.md for full mapping.
    // =========================================================================
    enum class Category : uint8_t
    {
        OneHandWeapons,   // 0xADC7 - 0xADDE
        TwoHandWeapons,   // 0xADDF - 0xADED, 0xAE05
        Shields,          // 0xADEE - 0xADF0
        Bows,             // 0xADF1 - 0xADF8
        SpecialWeapons,   // 0xADF9 - 0xAE02 (musket, bomb, fan, etc.)
        Tools,            // 0xAE06 - 0xAE1D, 0x0F4F
        Lanterns,         // 0xAE1E - 0xAE20
        COUNT
    };

    inline constexpr std::size_t CATEGORY_COUNT =
        static_cast<std::size_t>(Category::COUNT);

    constexpr std::string_view category_section(Category cat)
    {
        constexpr std::string_view names[] = {
            "OneHandWeapons", "TwoHandWeapons", "Shields", "Bows",
            "SpecialWeapons", "Tools", "Lanterns"};
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

    // =========================================================================
    // Per-category runtime state
    // =========================================================================
    struct CategoryState
    {
        std::atomic<bool> enabled{true};
        std::atomic<bool> hidden{false};
        DMK::Config::KeyComboList toggleHotkeyCombos;
        DMK::Config::KeyComboList showHotkeyCombos;
        DMK::Config::KeyComboList hideHotkeyCombos;
    };

    std::array<CategoryState, CATEGORY_COUNT>& category_states();

    // =========================================================================
    // Runtime hash resolution
    //
    // Part names are resolved to IndexedStringA hash IDs at runtime by
    // scanning the game's string table. This eliminates the need to update
    // hardcoded hash values when the game is patched.
    // =========================================================================

    /// Supply runtime-resolved name-to-hash mappings from the IndexedStringA table.
    /// Must be called before load_config / register_parts for runtime resolution
    /// to take effect. If not called, compile-time fallback hashes are used.
    void set_runtime_hashes(std::unordered_map<std::string, uint32_t>&& nameToHash);

    /// Returns fallback hashes for known parts not present in the given map.
    /// Used by the table scanner to do targeted probes for outlier hashes.
    std::vector<std::pair<std::string, uint32_t>> get_unresolved_fallbacks(
        const std::unordered_map<std::string, uint32_t>& resolved);

    /// Returns the minimum hash value of the contiguous block (for fast range checks).
    /// Loaded with relaxed ordering — stale bounds only widen the range check,
    /// causing harmless extra map lookups rather than missed classifications.
    uint32_t hash_range_min() noexcept;

    /// Returns the maximum hash value of the contiguous block (for fast range checks).
    /// Loaded with relaxed ordering — see hash_range_min() rationale.
    uint32_t hash_range_max() noexcept;

    /// Returns true if the hash is a registered outlier (outside the contiguous range).
    bool is_outlier_hash(uint32_t hash) noexcept;

    // =========================================================================
    // Part classification
    //
    // Parts are loaded exclusively from the INI file. Each category's "Parts"
    // key contains a comma-separated list of part names or raw hex IDs.
    // =========================================================================

    /// Parse a "Parts" string and register all contained IDs for the given category.
    void register_parts(Category cat, const std::string& partsStr);

    /// Finalize the lookup map and compute hash range bounds.
    /// Call after all register_parts().
    void build_part_lookup();

    /// Re-resolve part names against current runtime hashes and rebuild
    /// the lookup map.  Used when the deferred table scan completes after
    /// config has already been loaded with fallback hashes.
    void rebuild_part_lookup();

    /// Classify a part hash into a category.  Returns std::nullopt if not tracked.
    std::optional<Category> classify_part(uint32_t partHash);

    /// Returns true if a specific category is currently hidden (and enabled).
    bool is_category_hidden(Category cat);

    /// Returns the registered part-hash to category map by reference.
    /// The underlying buffer is double-buffered: readers see the active map
    /// while rebuild_part_lookup() writes to the inactive one and flips.
    /// Safe as long as iteration does not overlap a second rebuild (at most
    /// one deferred rebuild occurs, before hooks are active).
    const std::unordered_map<uint32_t, Category>& get_part_map();

} // namespace EquipHide

