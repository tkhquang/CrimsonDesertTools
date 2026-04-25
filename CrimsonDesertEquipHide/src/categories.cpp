#include "categories.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

namespace EquipHide
{
    static std::array<CategoryState, CATEGORY_COUNT> s_states{};

    std::array<CategoryState, CATEGORY_COUNT>& category_states()
    {
        return s_states;
    }

    /* Part names and categories. Hashes resolved at runtime via
       IndexedStringA table scan. See .idea/research/ for full mapping. */

    // --- Name -> hash table ---
    struct NamedPart
    {
        const char* name;
        Category    cat;
    };

    // clang-format off
    static constexpr NamedPart k_allParts[] = {
        // 1H Weapons
        {"CD_MainWeapon_Sword_R",        Category::OneHandWeapons},
        {"CD_MainWeapon_Sword_IN_R",     Category::OneHandWeapons},
        {"CD_MainWeapon_Sword_L",        Category::OneHandWeapons},
        {"CD_MainWeapon_Sword_IN_L",     Category::OneHandWeapons},
        {"CD_MainWeapon_Dagger_R",       Category::OneHandWeapons},
        {"CD_MainWeapon_Dagger_IN_R",    Category::OneHandWeapons},
        {"CD_MainWeapon_Dagger_L",       Category::OneHandWeapons},
        {"CD_MainWeapon_Dagger_IN_L",    Category::OneHandWeapons},
        {"CD_MainWeapon_Axe_R",          Category::OneHandWeapons},
        {"CD_MainWeapon_Axe_L",          Category::OneHandWeapons},
        {"CD_MainWeapon_Mace_R",         Category::OneHandWeapons},
        {"CD_MainWeapon_Mace_L",         Category::OneHandWeapons},
        {"CD_MainWeapon_Hammer_R",       Category::OneHandWeapons},
        {"CD_MainWeapon_Flail_R",        Category::OneHandWeapons},
        {"CD_MainWeapon_Wand_R",         Category::OneHandWeapons},
        {"CD_MainWeapon_Bola",           Category::OneHandWeapons},
        {"CD_MainWeapon_Fist_R",         Category::OneHandWeapons},
        {"CD_MainWeapon_Fist_L",         Category::OneHandWeapons},
        {"CD_MainWeapon_HandCannon",     Category::OneHandWeapons},
        {"CD_MainWeapon_Fist_Hand",      Category::OneHandWeapons},
        {"CD_MainWeapon_Fist_Foot",      Category::OneHandWeapons},
        {"CD_MainWeapon_Lance",          Category::OneHandWeapons},
        {"CD_MainWeapon_Gauntlet",       Category::OneHandWeapons},
        {"CD_MainWeapon_Gauntlet_L",     Category::OneHandWeapons},
        {"CD_MainWeapon_Sword_R_Aux",    Category::OneHandWeapons},
        {"CD_MainWeapon_Sword_IN_R_Aux", Category::OneHandWeapons},
        // 2H Weapons
        {"CD_TwoHandWeapon_Sword",       Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Axe",         Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Axe_Aux",     Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Mace",        Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_WarHammer",   Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Hammer",      Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Cannon",      Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_CannonBall",  Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Thrower",     Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Spear",       Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Alebard",     Category::TwoHandWeapons},
        {"CD_MainWeapon_Pike",           Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Rod",         Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Flail",       Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_BlowPipe",    Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Scythe",      Category::TwoHandWeapons},
        {"CD_TwoHandWeapon_Flag",        Category::TwoHandWeapons},
        // Shields
        {"CD_MainWeapon_Shield_L",       Category::Shields},
        {"CD_MainWeapon_Shield_R",       Category::Shields},
        {"CD_MainWeapon_TowerShield_L",  Category::Shields},
        // Bows / Arrows
        {"CD_MainWeapon_Bow",            Category::Bows},
        {"CD_MainWeapon_Quiver",         Category::Bows},
        {"CD_MainWeapon_Quiver_Arw",     Category::Bows},
        {"CD_MainWeapon_Quiver_Arw_01",  Category::Bows},
        {"CD_MainWeapon_Quiver_Arw_02",  Category::Bows},
        {"CD_MainWeapon_Quiver_Arw_03",  Category::Bows},
        {"CD_MainWeapon_Arw",            Category::Bows},
        {"CD_MainWeapon_Arwline",        Category::Bows},
        {"CD_MainWeapon_Arw_IN",         Category::Bows},
        // Special / Ranged
        {"CD_MainWeapon_ArwHead",        Category::SpecialWeapons},
        {"CD_MainWeapon_CrossBow",       Category::SpecialWeapons},
        {"CD_MainWeapon_Pistol_R",       Category::SpecialWeapons},
        {"CD_MainWeapon_Pistol_L",       Category::SpecialWeapons},
        {"CD_MainWeapon_Musket",         Category::SpecialWeapons},
        {"CD_MainWeapon_Trap",           Category::SpecialWeapons},
        {"CD_MainWeapon_Bomb",           Category::SpecialWeapons},
        {"CD_MainWeapon_Fan",            Category::SpecialWeapons},
        {"CD_MainWeapon_ThrownSpear_R",  Category::SpecialWeapons},
        {"CD_MainWeapon_ThrownSpear_L",  Category::SpecialWeapons},
        {"CD_MainWeapon_Whip_R",         Category::SpecialWeapons},
        {"CD_MainWeapon_Parachute",      Category::SpecialWeapons},
        // Tools
        {"CD_Tool_FishingRod",           Category::Tools},
        {"CD_Tool",                      Category::Tools},
        {"CD_Tool_01",                   Category::Tools},
        {"CD_Tool_02",                   Category::Tools},
        {"CD_Tool_Axe",                  Category::Tools},
        {"CD_Tool_Hammer",               Category::Tools},
        {"CD_Tool_Saw",                  Category::Tools},
        {"CD_Tool_Hoe",                  Category::Tools},
        {"CD_Tool_Broom",                Category::Tools},
        {"CD_Tool_FarmScythe",           Category::Tools},
        {"CD_Tool_Hayfork",              Category::Tools},
        {"CD_Tool_Pickaxe",              Category::Tools},
        {"CD_Tool_Rake",                 Category::Tools},
        {"CD_Tool_Shovel",               Category::Tools},
        {"CD_Tool_Crutch",              Category::Tools},
        {"CD_Tool_FishingRod_Sub",       Category::Tools},
        {"CD_Tool_Shooter",              Category::Tools},
        {"CD_Tool_Flute",                Category::Tools},
        {"CD_Tool_FireCan",              Category::Tools},
        {"CD_Tool_Cigarette",            Category::Tools},
        {"CD_Tool_Sprayer",              Category::Tools},
        {"CD_Tool_HandDrum",             Category::Tools},
        {"CD_Tool_DrumStick_R",          Category::Tools},
        {"CD_Tool_DrumStick_L",          Category::Tools},
        {"CD_Tool_Torch",                Category::Tools},
        {"CD_Tool_Pan",                  Category::Tools},
        {"CD_Tool_Trumpet",              Category::Tools},
        {"CD_Tool_Pipe",                 Category::Tools},
        {"CD_Tool_Book",                 Category::Tools},
        // Lanterns
        {"CD_Tool_Hyperspace_RemoteControl", Category::Lanterns},
        {"CD_Lantern",                   Category::Lanterns},
        {"CD_Lantern_Ring",              Category::Lanterns},
        // Helm (armor)
        {"CD_Helm",                      Category::Helm},
        {"CD_Helm_Acc",                  Category::Helm},
        {"CD_Helm_Acc_01",              Category::Helm},
        {"CD_Helm_Acc_02",              Category::Helm},
        {"CD_Helm_Small",                Category::Helm},
        {"CD_Helm_Visione_Belt",         Category::Helm},
        {"CD_Helm_Flight",               Category::Helm},
        // Chest (armor)
        {"CD_Upperbody",                 Category::Chest},
        {"CD_Upperbody_Acc",             Category::Chest},
        {"CD_Upperbody_Acc_01",          Category::Chest},
        {"CD_Upperbody_Acc_02",          Category::Chest},
        {"CD_Vest",                      Category::Chest},
        {"CD_Vest_Acc",                  Category::Chest},
        {"CD_Vest_Acc_01",               Category::Chest},
        {"CD_Jacket",                    Category::Chest},
        {"CD_Jacket_Acc",                Category::Chest},
        {"CD_Jacket_Acc_01",             Category::Chest},
        // Legs (armor)
        {"CD_Lowerbody",                 Category::Legs},
        {"CD_Lowerbody_Acc",             Category::Legs},
        {"CD_Underwear",                 Category::Underwear},
        // Gloves (armor)
        {"CD_Hand",                      Category::Gloves},
        {"CD_Hand_Acc",                  Category::Gloves},
        {"CD_Hand_Acc_01",               Category::Gloves},
        {"CD_Hand_Acc_02",               Category::Gloves},
        // Boots (armor)
        {"CD_Foot",                      Category::Boots},
        {"CD_Foot_Acc",                  Category::Boots},
        {"CD_Foot_Acc_01",               Category::Boots},
        // Cloak (armor)
        {"CD_Cloak",                     Category::Cloak},
        {"CD_Cloak_Acc",                 Category::Cloak},
        {"CD_Cloak_Acc_01",              Category::Cloak},
        {"CD_Cloak_Acc_02",              Category::Cloak},
        {"CD_Cloak_Shoulder",            Category::Cloak},
        {"CD_Cloak_Flight",              Category::Cloak},
        {"CD_Cloak_Flight_01",           Category::Cloak},
        {"CD_Cloak_Flight_02",           Category::Cloak},
        {"CD_Cloak_Flight_03",           Category::Cloak},
        // Shoulder (armor)
        {"CD_Shoulder",                  Category::Shoulder},
        {"CD_Shoulder_Under",            Category::Shoulder},
        {"CD_Shoulder_Acc",              Category::Shoulder},
        {"CD_Shoulder_Acc_01",           Category::Shoulder},
        // Mask (armor)
        {"CD_Mask",                      Category::Mask},
        {"CD_Mask_Acc",                  Category::Mask},
        {"CD_Mask_Acc_01",               Category::Mask},
        // Glasses (armor)
        {"CD_Glasses",                   Category::Glasses},
        // Earrings
        {"CD_Earring_L",                 Category::Earrings},
        {"CD_Earring_R",                 Category::Earrings},
        // Rings
        {"CD_Ring_R",                    Category::Rings},
        {"CD_Ring_L",                    Category::Rings},
        // Necklace
        {"CD_Necklace",                  Category::Necklace},
        // Bags
        {"CD_Belt",                      Category::Bags},
        {"CD_Acc",                       Category::Bags},
        {"CD_Bag",                       Category::Bags},
        {"CD_Bag_Rocket",                Category::Bags},
        {"CD_Bag_For_Dock",              Category::Bags},
        {"CD_Bag_Belt_For_Dock",         Category::Bags},
        {"CD_Additional_For_Dock",       Category::Bags},
        {"CD_Bag_Small",                 Category::Bags},
        {"CD_Bag_Acc",                   Category::Bags},
        {"CD_Bag_Belt",                  Category::Bags},
        {"CD_Bag_Lantern",               Category::Bags},
        {"CD_Bag_Rack",                  Category::Bags},
    };
    // clang-format on

    // --- Runtime hash resolution state ---
    static std::unordered_map<std::string, uint32_t> s_runtimeHashes;
    static bool s_hasRuntimeHashes = false;

    /* Written during init (config load) before hooks; read by rebuild_part_lookup()
       on the background scan thread. Hook installation provides happens-before. */
    static std::string s_categoryParts[CATEGORY_COUNT];

    /* Per-character Parts overrides. Empty = inherit from s_categoryParts[cat].
       Written during config load and never thereafter, so no synchronisation needed
       beyond the existing s_rebuildMutex guard on rebuild_part_lookup. */
    static std::string s_categoryPartsPerChar[CATEGORY_COUNT][kCharIdxCount];

    /* Active character index. -1 = use base Parts only (pre-resolution / unknown). */
    static std::atomic<int> s_activeChar{-1};

    /* Serialises rebuild_part_lookup(): the deferred IndexedStringA scan thread and
       the player-detection char-swap poll can both trigger rebuilds concurrently. */
    static std::mutex s_rebuildMutex;

    std::string_view character_name_for_idx(std::size_t idx) noexcept
    {
        constexpr std::string_view names[] = {"Kliff", "Damiane", "Oongka"};
        static_assert(std::size(names) == kCharIdxCount,
                      "names[] must match kCharIdxCount");
        return (idx < kCharIdxCount) ? names[idx] : std::string_view{};
    }

    void set_per_char_parts(Category cat, std::size_t charIdx, std::string partsStr)
    {
        if (charIdx >= kCharIdxCount)
            return;
        s_categoryPartsPerChar[static_cast<std::size_t>(cat)][charIdx] = std::move(partsStr);
    }

    int get_active_character() noexcept
    {
        return s_activeChar.load(std::memory_order_acquire);
    }

    /** @brief Pick effective Parts for a category, honouring the active per-char override. */
    static const std::string& effective_parts_for_category(std::size_t catIdx) noexcept
    {
        const int active = s_activeChar.load(std::memory_order_acquire);
        if (active >= 0 && active < static_cast<int>(kCharIdxCount))
        {
            const auto& override_parts = s_categoryPartsPerChar[catIdx][active];
            if (!override_parts.empty())
                return override_parts;
        }
        return s_categoryParts[catIdx];
    }

    void set_runtime_hashes(std::unordered_map<std::string, uint32_t>&& nameToHash)
    {
        s_runtimeHashes = std::move(nameToHash);
        s_hasRuntimeHashes = true;
    }

    std::size_t total_part_count() noexcept
    {
        return std::size(k_allParts);
    }

    std::vector<std::string> get_unresolved_parts(
        const std::unordered_map<std::string, uint32_t>& resolved)
    {
        std::vector<std::string> result;
        for (const auto& p : k_allParts)
        {
            if (!resolved.count(p.name))
                result.emplace_back(p.name);
        }
        return result;
    }

    /**
     * @brief Build name->hash lookup, preferring runtime-resolved hashes over fallbacks.
     * @details Written during init before hooks; read by rebuild_part_lookup()
     *          after deferred scan. Hook installation provides happens-before.
     */
    static std::unordered_map<std::string, uint32_t> s_nameToHash;
    static bool s_nameToHashBuilt = false;

    static const std::unordered_map<std::string, uint32_t>& name_to_hash_map()
    {
        if (!s_nameToHashBuilt)
        {
            auto& logger = DMK::Logger::get_instance();

            if (s_hasRuntimeHashes)
            {
                s_nameToHash = s_runtimeHashes;

                int resolved = 0;
                for (const auto& p : k_allParts)
                {
                    if (s_nameToHash.count(p.name))
                        ++resolved;
                }
                logger.info("Hash resolution: {}/{} runtime",
                            resolved, std::size(k_allParts));
            }
            else
            {
                logger.info("No runtime hashes available, part map empty "
                            "(pending deferred scan)");
            }
            s_nameToHashBuilt = true;
        }
        return s_nameToHash;
    }

    static void invalidate_name_to_hash_map()
    {
        s_nameToHash.clear();
        s_nameToHashBuilt = false;
    }

    // --- Dynamic hash range bounds ---
    static std::atomic<uint32_t> s_hashRangeMin{0};
    static std::atomic<uint32_t> s_hashRangeMax{0};

    uint32_t hash_range_min() noexcept
    {
        return s_hashRangeMin.load(std::memory_order_relaxed);
    }

    uint32_t hash_range_max() noexcept
    {
        return s_hashRangeMax.load(std::memory_order_relaxed);
    }

    std::string default_parts_string(Category cat)
    {
        std::string result;
        for (const auto &p : k_allParts)
        {
            if (p.cat != cat)
                continue;
            if (!result.empty())
                result += ", ";
            result += p.name;
        }
        return result;
    }

    // --- Parts parser + lookup map ---
    static std::unordered_map<uint32_t, CategoryMask> s_partMaps[2];
    static std::atomic<int> s_activeMap{0};

    /**
     * @brief Flat lookup table for the contiguous hash range 0xAD00-0xBFFF.
     * @details Bounds check + single array read (~3 cycles) replaces
     *          unordered_map lookup (~20 cycles). Value: 0 = unclassified.
     */
    static constexpr uint32_t k_flatBase  = 0xAC00;
    static constexpr uint32_t k_flatEnd   = 0xCFFF;
    static constexpr uint32_t k_flatSize  = k_flatEnd - k_flatBase + 1;

    static std::array<CategoryMask, k_flatSize> s_flatTables[2]{};

    struct OutlierEntry { uint32_t hash; CategoryMask mask; };
    static std::vector<OutlierEntry> s_outlierTables[2];

    /**
     * @brief 64K-bit classification bitset (8 KB) covering hash values 0x0000-0xFFFF.
     * @details Bit N set = hash N has a classification entry. Single memory
     *          access replaces range check + outlier scan.
     */
    static constexpr std::size_t k_bitsetWords = 1024;
    static std::array<uint64_t, k_bitsetWords> s_classifyBitsets[2]{};

    /**
     * @brief Parse a Parts= string and write classification entries into
     *        the supplied target map.
     * @details Mirrors the body of register_parts() but operates on an
     *          arbitrary map argument so the same token-parse logic can
     *          populate either the legacy active-map double-buffer or a
     *          per-character map without code duplication. Logging is
     *          suppressed to avoid the per-character build loop spamming
     *          the same warnings the active-map build already emits.
     */
    static void register_parts_into(
        Category cat,
        const std::string &partsStr,
        std::unordered_map<uint32_t, CategoryMask> &target)
    {
        const auto bit = category_bit(cat);

        // Clear prior entries for this category from the target map so
        // a per-character rebuild that drops a part still reflects the
        // removal in the per-character classification.
        for (auto it = target.begin(); it != target.end();)
        {
            it->second &= ~bit;
            if (it->second == 0)
                it = target.erase(it);
            else
                ++it;
        }

        if (partsStr.find_first_not_of(" ,") == std::string::npos)
            return;

        // NONE sentinel: explicitly disables the category for this scope
        // (per-character override path). Silent return.
        {
            auto first = partsStr.find_first_not_of(" \t,");
            auto last  = partsStr.find_last_not_of(" \t,");
            if (first != std::string::npos && last != std::string::npos &&
                (last - first + 1) == 4)
            {
                auto ch = [&](std::size_t i) {
                    char c = partsStr[first + i];
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
                };
                if (ch(0) == 'n' && ch(1) == 'o' && ch(2) == 'n' && ch(3) == 'e')
                    return;
            }
        }

        const auto &nameMap = name_to_hash_map();
        if (nameMap.empty())
            return;

        std::size_t pos = 0;
        while (pos < partsStr.size())
        {
            while (pos < partsStr.size() && (partsStr[pos] == ' ' || partsStr[pos] == ','))
                ++pos;
            if (pos >= partsStr.size())
                break;

            auto end = partsStr.find(',', pos);
            if (end == std::string::npos)
                end = partsStr.size();

            std::string token = partsStr.substr(pos, end - pos);
            pos = end;

            while (!token.empty() && token.back() == ' ')
                token.pop_back();
            while (!token.empty() && token.front() == ' ')
                token.erase(token.begin());

            if (token.empty())
                continue;

            auto it = nameMap.find(token);
            if (it != nameMap.end())
            {
                if (it->second == 0)
                    continue;
                target[it->second] |= bit;
                continue;
            }

            if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            {
                try
                {
                    auto id = static_cast<uint32_t>(
                        std::stoul(token.substr(2), nullptr, 16));
                    target[id] |= bit;
                }
                catch (...)
                {
                    // Malformed hex IDs are reported by the active-map
                    // build (register_parts) so suppressing the warning
                    // here just avoids duplication.
                }
            }
        }
    }

    void register_parts(Category cat, const std::string& partsStr, bool storeBase)
    {
        if (storeBase)
            s_categoryParts[static_cast<std::size_t>(cat)] = partsStr;

        auto& logger = DMK::Logger::get_instance();
        auto& writeMap = s_partMaps[1 - s_activeMap.load(std::memory_order_relaxed)];

        // Clear previous entries for this category so that a second callback
        // (e.g. INI override after default registration) fully replaces them.
        const auto bit = category_bit(cat);
        for (auto it = writeMap.begin(); it != writeMap.end(); )
        {
            it->second &= ~bit;
            if (it->second == 0)
                it = writeMap.erase(it);
            else
                ++it;
        }

        if (partsStr.find_first_not_of(" ,") == std::string::npos)
        {
            if (!partsStr.empty())
                logger.warning("{}: no parts configured (check INI file)", category_section(cat));
            return;
        }

        /* NONE sentinel -- case-insensitive, trimmed -- explicitly disables the
           category (commonly used in per-character overrides like
           [Lanterns:Damiane] Parts = NONE). Silent return, no warning. */
        {
            auto first = partsStr.find_first_not_of(" \t,");
            auto last  = partsStr.find_last_not_of(" \t,");
            if (first != std::string::npos && last != std::string::npos &&
                (last - first + 1) == 4)
            {
                auto ch = [&](std::size_t i) {
                    char c = partsStr[first + i];
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
                };
                if (ch(0) == 'n' && ch(1) == 'o' && ch(2) == 'n' && ch(3) == 'e')
                {
                    logger.debug("{}: NONE (category explicitly disabled for this scope)",
                                 category_section(cat));
                    return;
                }
            }
        }

        const auto& nameMap = name_to_hash_map();

        // No runtime hashes yet -- store names for deferred resolution only.
        // rebuild_part_lookup() will re-resolve after the scan completes.
        if (nameMap.empty())
            return;

        std::size_t pos = 0;
        while (pos < partsStr.size())
        {
            while (pos < partsStr.size() && (partsStr[pos] == ' ' || partsStr[pos] == ','))
                ++pos;
            if (pos >= partsStr.size())
                break;

            auto end = partsStr.find(',', pos);
            if (end == std::string::npos)
                end = partsStr.size();

            std::string token = partsStr.substr(pos, end - pos);
            pos = end;

            while (!token.empty() && token.back() == ' ')
                token.pop_back();
            while (!token.empty() && token.front() == ' ')
                token.erase(token.begin());

            if (token.empty())
                continue;

            auto it = nameMap.find(token);
            if (it != nameMap.end())
            {
                if (it->second == 0)
                {
                    if (storeBase)
                        logger.trace("  {} skipped {} (no stable hash)", category_section(cat), token);
                    continue;
                }
                writeMap[it->second] |= category_bit(cat);
                if (storeBase)
                    logger.trace("  {} += {} (0x{:04X})", category_section(cat), token, it->second);
                continue;
            }

            if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            {
                try
                {
                    auto id = static_cast<uint32_t>(std::stoul(token.substr(2), nullptr, 16));
                    writeMap[id] |= category_bit(cat);
                    if (storeBase)
                        logger.trace("  {} += 0x{:04X} (raw)", category_section(cat), id);
                    continue;
                }
                catch (...)
                {
                    logger.warning("  {} : malformed hex ID '{}'", category_section(cat), token);
                }
            }

            logger.debug("  {} : unknown part '{}' (pending runtime scan?)",
                         category_section(cat), token);
        }
    }

    // ---------------------------------------------------------------------
    // Per-character part maps (Phase 3 option (a) -- per-character
    // unordered_map). Rationale: hot path is classify_part_for() called
    // from apply_direct_vis_write per (vis_ctrl, hash) pair on every
    // schedule. Option (b) would tag every entry of the merged map with
    // an array<CategoryMask, kCharIdxCount> (12 bytes per value vs 4),
    // tripling the cache footprint of the inner walk that already
    // dominates direct-write latency. Option (a) pays a small rebuild
    // cost on character swap / INI reload (rare, off the hot path) in
    // exchange for one map lookup keyed on charIdx during direct write.
    //
    // The per-character maps are built alongside the active-map double-
    // buffer flip in build_part_lookup() so all consumers see the same
    // generation. Each is keyed (hash -> CategoryMask) exactly like the
    // legacy maps.
    // ---------------------------------------------------------------------
    static std::unordered_map<uint32_t, CategoryMask>
        s_partMapsPerChar[kCharIdxCount];

    // Per-character flat tables and bitsets, mirroring the active-map
    // fast paths (flat-table for the contiguous range, bitset for the
    // needs_classification_for fast filter). Outliers are searched off
    // the per-character unordered_map directly because the per-char
    // outlier count is small enough that maintaining a sorted vector
    // alongside is not worth the rebuild cost.
    static std::array<CategoryMask, k_flatSize>
        s_flatTablesPerChar[kCharIdxCount]{};
    static std::array<uint64_t, k_bitsetWords>
        s_classifyBitsetsPerChar[kCharIdxCount]{};

    // --- Outlier hash set ---
    static constexpr std::size_t k_maxOutliers = 8;
    static std::atomic<uint32_t> s_outliers[k_maxOutliers]{};
    static std::atomic<int>      s_outlierCount{0};

    bool is_outlier_hash(uint32_t hash) noexcept
    {
        const auto n = s_outlierCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
        {
            if (s_outliers[i].load(std::memory_order_relaxed) == hash)
                return true;
        }
        return false;
    }

    void build_part_lookup()
    {
        const auto writeIdx = 1 - s_activeMap.load(std::memory_order_relaxed);
        const auto& writeMap = s_partMaps[writeIdx];

        auto& flatTable = s_flatTables[writeIdx];
        auto& outlierTable = s_outlierTables[writeIdx];
        auto& bitset = s_classifyBitsets[writeIdx];

        flatTable.fill(0);
        outlierTable.clear();
        bitset.fill(0);

        for (const auto& [hash, mask] : writeMap)
        {
            if (hash >= k_flatBase && hash <= k_flatEnd)
            {
                flatTable[hash - k_flatBase] = mask;
            }
            else
            {
                outlierTable.push_back({hash, mask});
            }

            if (hash < 0x10000)
                bitset[hash / 64] |= (1ULL << (hash % 64));
        }

        std::sort(outlierTable.begin(), outlierTable.end(),
                  [](const OutlierEntry& a, const OutlierEntry& b)
                  { return a.hash < b.hash; });

        std::vector<uint32_t> hashes;
        hashes.reserve(writeMap.size());
        for (const auto& [hash, mask] : writeMap)
            hashes.push_back(hash);

        std::sort(hashes.begin(), hashes.end());

        uint32_t rangeMin = 0;
        uint32_t rangeMax = 0;
        int outlierCount = 0;

        if (!hashes.empty())
        {
            constexpr uint32_t k_gapThreshold = 0x100;
            std::size_t bestStart = 0;
            std::size_t bestLen   = 1;
            std::size_t curStart  = 0;

            for (std::size_t i = 1; i < hashes.size(); ++i)
            {
                if (hashes[i] - hashes[i - 1] > k_gapThreshold)
                    curStart = i;
                if (i - curStart + 1 > bestLen)
                {
                    bestStart = curStart;
                    bestLen   = i - curStart + 1;
                }
            }

            rangeMin = hashes[bestStart];
            rangeMax = hashes[bestStart + bestLen - 1];

            for (std::size_t i = 0; i < hashes.size(); ++i)
            {
                if (i < bestStart || i >= bestStart + bestLen)
                {
                    if (outlierCount < static_cast<int>(k_maxOutliers))
                        s_outliers[outlierCount++].store(
                            hashes[i], std::memory_order_relaxed);
                }
            }
        }

        s_hashRangeMin.store(rangeMin, std::memory_order_relaxed);
        s_hashRangeMax.store(rangeMax, std::memory_order_relaxed);
        s_outlierCount.store(outlierCount, std::memory_order_release);

        auto& logger = DMK::Logger::get_instance();
        const char *verb = s_nameToHashBuilt ? "rebuilt" : "built";
        logger.info("Part lookup {}: {} entries across {} categories",
                     verb, writeMap.size(), CATEGORY_COUNT);
        if (!writeMap.empty())
        {
            logger.info("Hash range: 0x{:X} - 0x{:X} ({} outliers)",
                         rangeMin, rangeMax, outlierCount);
            for (int i = 0; i < outlierCount; ++i)
                logger.debug("  outlier: 0x{:X}",
                              s_outliers[i].load(std::memory_order_relaxed));
        }

        s_activeMap.fetch_xor(1, std::memory_order_release);
    }

    /**
     * @brief Rebuild the per-character classification maps + flat tables
     *        + bitsets for every supported protagonist.
     * @details Independent of the active-map double-buffer flip so the
     *          per-character maps are a stable side store: a swap to a
     *          different active character does not have to relink the
     *          per-character data, only the read pointer for the legacy
     *          active-map fast paths. Called from rebuild_part_lookup
     *          while s_rebuildMutex is held, so concurrent reads of the
     *          per-character data through the lookup helpers are
     *          serialised by the rebuild path.
     */
    static void build_per_char_part_maps()
    {
        for (std::size_t c = 0; c < kCharIdxCount; ++c)
        {
            auto &map = s_partMapsPerChar[c];
            map.clear();

            for (std::size_t catIdx = 0; catIdx < CATEGORY_COUNT; ++catIdx)
            {
                // Effective Parts for character c = the per-character
                // override if non-empty, else the base [Section] Parts.
                // Mirrors effective_parts_for_category() but parameterised
                // on c instead of the active character.
                const auto &override_parts = s_categoryPartsPerChar[catIdx][c];
                const auto &effective =
                    !override_parts.empty()
                        ? override_parts
                        : s_categoryParts[catIdx];
                if (effective.empty())
                    continue;
                register_parts_into(static_cast<Category>(catIdx),
                                    effective, map);
            }

            // Rebuild the per-character flat table + bitset from the
            // freshly-populated map. Same flat-base / flat-end window as
            // the active-map path so the contiguous-range fast path is
            // structurally identical.
            auto &flat = s_flatTablesPerChar[c];
            auto &bits = s_classifyBitsetsPerChar[c];
            flat.fill(0);
            bits.fill(0);

            for (const auto &[hash, mask] : map)
            {
                if (hash >= k_flatBase && hash <= k_flatEnd)
                    flat[hash - k_flatBase] = mask;
                if (hash < 0x10000)
                    bits[hash / 64] |= (1ULL << (hash % 64));
            }
        }
    }

    void rebuild_part_lookup()
    {
        std::lock_guard<std::mutex> lock(s_rebuildMutex);
        invalidate_name_to_hash_map();
        s_partMaps[1 - s_activeMap.load(std::memory_order_relaxed)].clear();

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto& parts = effective_parts_for_category(i);
            if (!parts.empty())
                register_parts(static_cast<Category>(i), parts, /*storeBase=*/false);
        }

        build_part_lookup();
        // Per-character maps are an additive structure: the active-map
        // double-buffer above keeps the legacy (active-character)
        // semantics alive for callers that have not been migrated, while
        // the per-character maps make swap-time identity preserved at
        // each vis ctrl. Building them here ensures both views stay in
        // sync across every rebuild trigger (init, INI auto-reload,
        // set_active_character, deferred IndexedString scan completion).
        build_per_char_part_maps();
    }

    void set_active_character(int newIdx)
    {
        const int clamped =
            (newIdx < -1 || newIdx >= static_cast<int>(kCharIdxCount)) ? -1 : newIdx;
        const int prev = s_activeChar.exchange(clamped, std::memory_order_acq_rel);
        if (prev == clamped)
            return;

        auto& logger = DMK::Logger::get_instance();
        if (clamped >= 0)
            logger.info("Active character -> {} (idx={})",
                        character_name_for_idx(static_cast<std::size_t>(clamped)),
                        clamped);
        else
            logger.info("Active character -> (base, no override)");

        rebuild_part_lookup();
    }

    CategoryMask classify_part(uint32_t hash)
    {
        const auto idx = s_activeMap.load(std::memory_order_acquire);

        // Fast path: flat table lookup for the contiguous hash range
        if (hash >= k_flatBase && hash <= k_flatEnd)
            return s_flatTables[idx][hash - k_flatBase];

        // Slow path: binary search through sorted outlier array
        const auto& outliers = s_outlierTables[idx];
        if (!outliers.empty())
        {
            auto it = std::lower_bound(
                outliers.begin(), outliers.end(), hash,
                [](const OutlierEntry& e, uint32_t h) { return e.hash < h; });
            if (it != outliers.end() && it->hash == hash)
                return it->mask;
        }

        return 0;
    }

    bool needs_classification(uint32_t hash) noexcept
    {
        if (hash >= 0x10000)
            return false;
        const auto idx = s_activeMap.load(std::memory_order_acquire);
        return (s_classifyBitsets[idx][hash / 64] & (1ULL << (hash % 64))) != 0;
    }

    bool is_category_hidden(Category cat)
    {
        const auto idx = static_cast<std::size_t>(cat);
        const auto& st = s_states[idx];
        return st.enabled.load(std::memory_order_relaxed) &&
               st.hidden.load(std::memory_order_relaxed);
    }

    // Single atomic load replaces per-category iteration in the hot path.
    static std::atomic<CategoryMask> s_hiddenMask{0};
    static std::atomic<CategoryMask> s_presetHiddenMask{0};
    static std::atomic<CategoryMask> s_activePresetMask{0};

    void update_hidden_mask()
    {
        CategoryMask hidden = 0;
        CategoryMask presetHidden = 0;
        CategoryMask activePreset = 0;

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto &st = s_states[i];
            const bool enabled = st.enabled.load(std::memory_order_relaxed);
            const bool isHidden = st.hidden.load(std::memory_order_relaxed);
            const auto bit = CategoryMask{1} << i;

            if (is_user_preset(static_cast<Category>(i)))
            {
                if (enabled)
                {
                    activePreset |= bit;
                    if (isHidden)
                        presetHidden |= bit;
                }
            }
            else
            {
                if (enabled && isHidden)
                    hidden |= bit;
            }
        }

        s_activePresetMask.store(activePreset, std::memory_order_relaxed);
        s_presetHiddenMask.store(presetHidden, std::memory_order_relaxed);
        s_hiddenMask.store(hidden, std::memory_order_relaxed);
    }

    bool is_any_category_hidden(CategoryMask mask)
    {
        const auto presetOverlap = mask & s_activePresetMask.load(std::memory_order_relaxed);
        if (presetOverlap != 0)
            return (presetOverlap & s_presetHiddenMask.load(std::memory_order_relaxed)) != 0;
        return (mask & s_hiddenMask.load(std::memory_order_relaxed)) != 0;
    }

    const std::unordered_map<uint32_t, CategoryMask>& get_part_map()
    {
        return s_partMaps[s_activeMap.load(std::memory_order_acquire)];
    }

    bool needs_classification_for(uint32_t hash, int charIdx) noexcept
    {
        if (charIdx < 0 || charIdx >= static_cast<int>(kCharIdxCount))
            return needs_classification(hash);
        if (hash >= 0x10000)
            return false;
        const auto &bits = s_classifyBitsetsPerChar[
            static_cast<std::size_t>(charIdx)];
        return (bits[hash / 64] & (1ULL << (hash % 64))) != 0;
    }

    CategoryMask classify_part_for(uint32_t hash, int charIdx) noexcept
    {
        if (charIdx < 0 || charIdx >= static_cast<int>(kCharIdxCount))
            return classify_part(hash);

        const auto c = static_cast<std::size_t>(charIdx);

        // Flat-table fast path for the contiguous range, mirroring the
        // active-map classify_part hot path.
        if (hash >= k_flatBase && hash <= k_flatEnd)
            return s_flatTablesPerChar[c][hash - k_flatBase];

        // Outliers: search the per-character unordered_map directly.
        // Keeping a sorted outlier vector per character would shave a
        // few cycles off the rare miss but doubles the per-rebuild cost
        // for a path that fires only on outlier hashes.
        const auto &map = s_partMapsPerChar[c];
        const auto it = map.find(hash);
        return (it != map.end()) ? it->second : CategoryMask{0};
    }

    const std::unordered_map<uint32_t, CategoryMask> &
    get_part_map_for(int charIdx) noexcept
    {
        // Out-of-range fallback returns the active-character map so any
        // accidental misuse stays observably correct (legacy behaviour)
        // rather than crashing.
        if (charIdx < 0 || charIdx >= static_cast<int>(kCharIdxCount))
            return get_part_map();
        return s_partMapsPerChar[static_cast<std::size_t>(charIdx)];
    }

    bool is_any_category_hidden_for(CategoryMask mask, int charIdx) noexcept
    {
        // The hidden-state masks (s_activePresetMask, s_presetHiddenMask,
        // s_hiddenMask) are global -- they reflect the user's per-
        // category Hidden / Enabled toggles, NOT the per-character Parts
        // overrides. The per-character split happens in classify_part_for
        // (different hashes -> different category bits). Hidden-state
        // computation can therefore reuse the active-character helper
        // verbatim once the caller has classified through the per-
        // character map.
        (void)charIdx;
        return is_any_category_hidden(mask);
    }

} // namespace EquipHide
