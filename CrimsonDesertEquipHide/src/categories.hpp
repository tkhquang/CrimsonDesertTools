#pragma once

#include <DetourModKit.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

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
        return names[static_cast<std::size_t>(cat)];
    }

    // =========================================================================
    // Per-category runtime state
    // =========================================================================
    struct CategoryState
    {
        std::atomic<bool> enabled{true};
        std::atomic<bool> hidden{false};
        DMK::Config::KeyComboList hotkeyCombos;
    };

    std::array<CategoryState, CATEGORY_COUNT>& category_states();

    // =========================================================================
    // Part classification
    //
    // Default ranges are hardcoded but can be overridden via INI "Parts" key.
    // Format: "0xADC7-0xADDE, 0xAE05" (ranges and individual hex IDs).
    // =========================================================================

    /// Returns the default "Parts" string for a category.
    std::string_view default_parts(Category cat);

    /// Parse a "Parts" string and register all contained IDs for the given category.
    void register_parts(Category cat, const std::string& partsStr);

    /// Build the lookup map from all registered parts. Call after all register_parts().
    void build_part_lookup();

    /// Classify a part hash into a category.  Returns std::nullopt if not tracked.
    std::optional<Category> classify_part(uint32_t partHash);

    /// Returns true if a specific category is currently hidden (and enabled).
    bool is_category_hidden(Category cat);

} // namespace EquipHide

