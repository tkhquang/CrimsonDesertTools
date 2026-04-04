#include "categories.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
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
        {"CD_Underwear",                 Category::Legs},
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

    void register_parts(Category cat, const std::string& partsStr)
    {
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

        const auto& nameMap = name_to_hash_map();

        // No runtime hashes yet — store names for deferred resolution only.
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
                    logger.trace("  {} skipped {} (no stable hash)", category_section(cat), token);
                    continue;
                }
                writeMap[it->second] |= category_bit(cat);
                logger.trace("  {} += {} (0x{:04X})", category_section(cat), token, it->second);
                continue;
            }

            if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            {
                try
                {
                    auto id = static_cast<uint32_t>(std::stoul(token.substr(2), nullptr, 16));
                    writeMap[id] |= category_bit(cat);
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

    void rebuild_part_lookup()
    {
        invalidate_name_to_hash_map();
        s_partMaps[1 - s_activeMap.load(std::memory_order_relaxed)].clear();

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            if (!s_categoryParts[i].empty())
                register_parts(static_cast<Category>(i), s_categoryParts[i]);
        }

        build_part_lookup();
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

} // namespace EquipHide
