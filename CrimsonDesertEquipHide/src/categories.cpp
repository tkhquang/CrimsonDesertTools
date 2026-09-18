#include "categories.hpp"

#include <DetourModKit/defines.hpp>
#include <DetourModKit/format.hpp>
#include <DetourModKit/logger.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace EquipHide
{
    static std::array<CategoryState, CATEGORY_COUNT> s_states{};

    std::array<CategoryState, CATEGORY_COUNT> &category_states() noexcept
    {
        return s_states;
    }

    // Part names and categories. Hashes resolve at runtime through the IndexedStringA table scan. See
    // .idea/research/ for the full mapping.

    // Name -> hash table
    struct NamedPart
    {
        const char *name;
        Category cat;
    };

    // clang-format off
    static constexpr NamedPart ALL_PARTS[] = {
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

    // Runtime hash resolution state
    static std::unordered_map<std::string, uint32_t> s_runtime_hashes;
    static bool s_has_runtime_hashes = false;

    // Written during init (config load) before hooks. Read by rebuild_part_lookup() on the background scan thread.
    // Hook installation provides happens-before.
    static std::string s_category_parts[CATEGORY_COUNT];

    // Per-character Parts overrides. Empty = inherit from s_category_parts[cat]. Written during config load and never
    // thereafter, so no synchronization is needed beyond the existing s_rebuild_mutex guard on rebuild_part_lookup.
    static std::string s_category_parts_per_char[CATEGORY_COUNT][k_charIdxCount];

    // Active character index. -1 = use base Parts only (pre-resolution / unknown).
    static std::atomic<int> s_active_char{-1};

    // Serializes rebuild_part_lookup(). The deferred IndexedStringA scan thread and the player-detection char-swap
    // poll can both trigger rebuilds concurrently.
    static std::mutex s_rebuild_mutex;

    std::string_view character_name_for_idx(std::size_t idx) noexcept
    {
        constexpr std::string_view names[] = {"Kliff", "Damiane", "Oongka"};
        static_assert(std::size(names) == k_charIdxCount, "names[] must match k_charIdxCount");
        return (idx < k_charIdxCount) ? names[idx] : std::string_view{};
    }

    void set_per_char_parts(Category cat, std::size_t char_idx, std::string parts_str)
    {
        if (char_idx >= k_charIdxCount)
            return;
        s_category_parts_per_char[static_cast<std::size_t>(cat)][char_idx] = std::move(parts_str);
    }

    /** @brief Pick effective Parts for a category, honoring the active per-char override. */
    static const std::string &effective_parts_for_category(std::size_t cat_idx) noexcept
    {
        const int active = s_active_char.load(std::memory_order_acquire);
        if (active >= 0 && active < static_cast<int>(k_charIdxCount))
        {
            const auto &override_parts = s_category_parts_per_char[cat_idx][active];
            if (!override_parts.empty())
                return override_parts;
        }
        return s_category_parts[cat_idx];
    }

    void set_runtime_hashes(std::unordered_map<std::string, uint32_t> &&name_to_hash)
    {
        s_runtime_hashes = std::move(name_to_hash);
        s_has_runtime_hashes = true;
    }

    std::size_t total_part_count() noexcept
    {
        return std::size(ALL_PARTS);
    }

    std::vector<std::string> get_unresolved_parts(const std::unordered_map<std::string, uint32_t> &resolved)
    {
        std::vector<std::string> result;
        for (const auto &p : ALL_PARTS)
        {
            if (!resolved.count(p.name))
                result.emplace_back(p.name);
        }
        return result;
    }

    // Written during init before hooks. Read by rebuild_part_lookup() after the deferred scan. Hook installation
    // provides happens-before.
    static std::unordered_map<std::string, uint32_t> s_name_to_hash;
    static bool s_name_to_hash_built = false;

    /** @brief Build the name to hash lookup, preferring runtime-resolved hashes over fallbacks. */
    static const std::unordered_map<std::string, uint32_t> &name_to_hash_map()
    {
        if (!s_name_to_hash_built)
        {
            if (s_has_runtime_hashes)
            {
                s_name_to_hash = s_runtime_hashes;
            }
            else
            {
                DMK::log().info("No runtime hashes available, part map empty (pending deferred scan)");
            }
            s_name_to_hash_built = true;
        }
        return s_name_to_hash;
    }

    static void invalidate_name_to_hash_map()
    {
        s_name_to_hash.clear();
        s_name_to_hash_built = false;
    }

    std::string default_parts_string(Category cat)
    {
        std::string result;
        for (const auto &p : ALL_PARTS)
        {
            if (p.cat != cat)
                continue;
            if (!result.empty())
                result += ", ";
            result += p.name;
        }
        return result;
    }

    // Parts parser + lookup map
    static std::unordered_map<uint32_t, CategoryMask> s_part_maps[2];
    static std::atomic<int> s_active_map{0};

    /**
     * @brief Flat lookup table for the contiguous hash range 0xAD00-0xBFFF.
     * @details Bounds check + single array read (~3 cycles) replaces unordered_map lookup (~20 cycles). Value: 0 =
     *          unclassified.
     */
    static constexpr uint32_t FLAT_BASE = 0xAC00;
    static constexpr uint32_t FLAT_END = 0xCFFF;
    static constexpr uint32_t FLAT_SIZE = FLAT_END - FLAT_BASE + 1;

    static std::array<CategoryMask, FLAT_SIZE> s_flat_tables[2]{};

    struct OutlierEntry
    {
        uint32_t hash;
        CategoryMask mask;
    };
    static std::vector<OutlierEntry> s_outlier_tables[2];

    /**
     * @brief 64K-bit classification bitset (8 KB) covering hash values 0x0000-0xFFFF.
     * @details Bit N set = hash N has a classification entry. Single memory access replaces range check + outlier scan.
     */
    static constexpr std::size_t BITSET_WORDS = 1024;
    static std::array<uint64_t, BITSET_WORDS> s_classify_bitsets[2]{};

    /**
     * @brief Parse a Parts= string and write classification entries into the supplied target map.
     * @details The single token parser behind both builds. The active-map build reports what it resolves, while the
     *          per-character build stays quiet because it re-resolves the same tokens once per protagonist, and an
     *          ungated report would multiply every line by k_charIdxCount on each rebuild.
     * @param cat The category whose bit every resolved hash carries.
     * @param parts_str The raw INI value: a comma-separated list of part names and 0x-prefixed hash literals.
     * @param target The map that receives the hash to mask entries.
     * @param diagnostics True to report an empty value, the NONE sentinel, a malformed hex ID, and an unknown part
     *                   name. The per-character build passes false so only the active-map build reports each token.
     * @param traces True to report every resolved token at Trace level.
     */
    static void parse_parts_into(
        Category cat,
        const std::string &parts_str,
        std::unordered_map<uint32_t, CategoryMask> &target,
        bool diagnostics,
        bool traces
    )
    {
        auto &logger = DMK::log();
        const auto bit = category_bit(cat);

        // Clear previous entries for this category so a second callback (an INI override after the default
        // registration) fully replaces them, and so a rebuild that drops a part also drops its classification.
        for (auto it = target.begin(); it != target.end();)
        {
            it->second &= ~bit;
            if (it->second == 0)
                it = target.erase(it);
            else
                ++it;
        }

        if (parts_str.find_first_not_of(" ,") == std::string::npos)
        {
            if (diagnostics && !parts_str.empty())
                logger.warning("{}: no parts configured (check INI file)", category_section(cat));
            return;
        }

        // NONE sentinel, case-insensitive and trimmed of spaces, tabs and commas, explicitly disables the category
        // for this scope. Commonly used in a per-character override such as [Lanterns:Damiane] Parts = NONE.
        {
            auto first = parts_str.find_first_not_of(" \t,");
            auto last = parts_str.find_last_not_of(" \t,");
            if (first != std::string::npos && last != std::string::npos && (last - first + 1) == 4)
            {
                auto ch = [&](std::size_t i)
                {
                    char c = parts_str[first + i];
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
                };
                if (ch(0) == 'n' && ch(1) == 'o' && ch(2) == 'n' && ch(3) == 'e')
                {
                    if (diagnostics)
                        logger.debug("{}: NONE (category explicitly disabled for this scope)", category_section(cat));
                    return;
                }
            }
        }

        const auto &name_map = name_to_hash_map();

        // No runtime hashes yet, so the names stay stored for deferred resolution only. rebuild_part_lookup()
        // re-resolves them after the scan completes.
        if (name_map.empty())
            return;

        std::size_t pos = 0;
        while (pos < parts_str.size())
        {
            while (pos < parts_str.size() && (parts_str[pos] == ' ' || parts_str[pos] == ','))
                ++pos;
            if (pos >= parts_str.size())
                break;

            auto end = parts_str.find(',', pos);
            if (end == std::string::npos)
                end = parts_str.size();

            const std::string token = DMK::string::trim(std::string_view{parts_str}.substr(pos, end - pos));
            pos = end;

            if (token.empty())
                continue;

            auto it = name_map.find(token);
            if (it != name_map.end())
            {
                if (it->second == 0)
                {
                    if (traces)
                        logger.trace("  {} skipped {} (no stable hash)", category_section(cat), token);
                    continue;
                }
                target[it->second] |= bit;
                if (traces)
                    logger.trace("  {} += {} (0x{:04X})", category_section(cat), token, it->second);
                continue;
            }

            if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            {
                try
                {
                    auto id = static_cast<uint32_t>(std::stoul(token.substr(2), nullptr, 16));
                    target[id] |= bit;
                    if (traces)
                        logger.trace("  {} += 0x{:04X} (raw)", category_section(cat), id);
                    continue;
                }
                catch (...)
                {
                    if (diagnostics)
                        logger.warning("  {} : malformed hex ID '{}'", category_section(cat), token);
                }
            }

            if (diagnostics)
                logger.debug("  {} : unknown part '{}' (pending runtime scan?)", category_section(cat), token);
        }
    }

    void register_parts(Category cat, const std::string &parts_str, bool store_base)
    {
        if (store_base)
            s_category_parts[static_cast<std::size_t>(cat)] = parts_str;

        auto &write_map = s_part_maps[1 - s_active_map.load(std::memory_order_relaxed)];
        parse_parts_into(cat, parts_str, write_map, /*diagnostics=*/true, /*traces=*/store_base);
    }

    // Per-character part maps, one separate unordered_map per protagonist. The hot path is classify_part_for(),
    // called from apply_direct_vis_write per (vis_ctrl, hash) pair on every schedule. A single merged map that tags
    // every entry with an array<CategoryMask, k_charIdxCount> (12 bytes per value against 4) triples the cache
    // footprint of the inner walk that already dominates direct-write latency.
    //
    // Separate maps instead pay a small rebuild cost on a character swap or an INI reload, both rare and off the hot
    // path, for one map lookup keyed on char_idx during a direct write. They are built alongside the active-map
    // double-buffer flip in build_part_lookup(), so every consumer sees the same generation, and each is keyed
    // (hash -> CategoryMask) exactly like the active-character map.
    static std::unordered_map<uint32_t, CategoryMask> s_part_maps_per_char[k_charIdxCount];

    // Per-character flat tables, mirroring the active-map flat-table fast path for the contiguous range. Outliers are
    // searched off the per-character unordered_map directly because the per-char outlier count is small enough that
    // maintaining a sorted vector alongside is not worth the rebuild cost.
    static std::array<CategoryMask, FLAT_SIZE> s_flat_tables_per_char[k_charIdxCount]{};

    // Outlier hash set
    static constexpr std::size_t MAX_OUTLIERS = 8;
    static std::atomic<uint32_t> s_outliers[MAX_OUTLIERS]{};
    static std::atomic<int> s_outlier_count{0};

    void build_part_lookup()
    {
        const auto write_idx = 1 - s_active_map.load(std::memory_order_relaxed);
        const auto &write_map = s_part_maps[write_idx];

        auto &flat_table = s_flat_tables[write_idx];
        auto &outlier_table = s_outlier_tables[write_idx];
        auto &bitset = s_classify_bitsets[write_idx];

        flat_table.fill(0);
        outlier_table.clear();
        bitset.fill(0);

        for (const auto &[hash, mask] : write_map)
        {
            if (hash >= FLAT_BASE && hash <= FLAT_END)
            {
                flat_table[hash - FLAT_BASE] = mask;
            }
            else
            {
                outlier_table.push_back({hash, mask});
            }

            if (hash < 0x10000)
                bitset[hash / 64] |= (1ULL << (hash % 64));
        }

        std::sort(
            outlier_table.begin(),
            outlier_table.end(),
            [](const OutlierEntry &a, const OutlierEntry &b) { return a.hash < b.hash; }
        );

        std::vector<uint32_t> hashes;
        hashes.reserve(write_map.size());
        for (const auto &[hash, mask] : write_map)
            hashes.push_back(hash);

        std::sort(hashes.begin(), hashes.end());

        uint32_t range_min = 0;
        uint32_t range_max = 0;
        int outlier_count = 0;

        if (!hashes.empty())
        {
            constexpr uint32_t gap_threshold = 0x100;
            std::size_t best_start = 0;
            std::size_t best_len = 1;
            std::size_t cur_start = 0;

            for (std::size_t i = 1; i < hashes.size(); ++i)
            {
                if (hashes[i] - hashes[i - 1] > gap_threshold)
                    cur_start = i;
                if (i - cur_start + 1 > best_len)
                {
                    best_start = cur_start;
                    best_len = i - cur_start + 1;
                }
            }

            range_min = hashes[best_start];
            range_max = hashes[best_start + best_len - 1];

            for (std::size_t i = 0; i < hashes.size(); ++i)
            {
                if (i < best_start || i >= best_start + best_len)
                {
                    if (outlier_count < static_cast<int>(MAX_OUTLIERS))
                        s_outliers[outlier_count++].store(hashes[i], std::memory_order_relaxed);
                }
            }
        }

        s_outlier_count.store(outlier_count, std::memory_order_release);

        auto &logger = DMK::log();
        // Skip the summary emit when the part map is empty. The empty case is the initial build that runs before any
        // runtime hash has been resolved; name_to_hash_map() already logs "No runtime hashes available, part map empty
        // (pending deferred scan)" for that state, so a "0 entries" companion here carries no extra signal.
        if (!write_map.empty())
        {
            const char *verb = s_name_to_hash_built ? "rebuilt" : "built";

            // Catalog parts with a runtime-hash mapping. Reported as the "{resolved}/{total} resolved" field of the
            // consolidated INFO line below.
            int resolved = 0;
            if (s_has_runtime_hashes)
            {
                for (const auto &p : ALL_PARTS)
                {
                    if (s_name_to_hash.count(p.name))
                        ++resolved;
                }
            }

            logger.info(
                "Part lookup {}: {} entries across {} categories ({}/{} resolved, range 0x{:X}-0x{:X}, {} outliers)",
                verb,
                write_map.size(),
                CATEGORY_COUNT,
                resolved,
                std::size(ALL_PARTS),
                range_min,
                range_max,
                outlier_count
            );

            if (outlier_count > 0 && logger.is_enabled(DMK::LogLevel::Debug))
            {
                std::string list;
                list.reserve(outlier_count * 8);
                for (int i = 0; i < outlier_count; ++i)
                {
                    if (i > 0)
                        list += ", ";
                    list += std::format("0x{:X}", s_outliers[i].load(std::memory_order_relaxed));
                }
                logger.debug("  outliers: {}", list);
            }
        }

        s_active_map.fetch_xor(1, std::memory_order_release);
    }

    /**
     * @brief Rebuild the per-character classification maps, flat tables and bitsets for every supported protagonist.
     * @details Independent of the active-map double-buffer flip, so the per-character maps are a stable side store. A
     *          swap to a different active character relinks only the read pointer for the active-character fast paths,
     *          never the per-character data. Called from rebuild_part_lookup while s_rebuild_mutex is held, so the
     *          rebuild path serializes concurrent reads of the per-character data through the lookup helpers.
     */
    static void build_per_char_part_maps()
    {
        for (std::size_t c = 0; c < k_charIdxCount; ++c)
        {
            auto &map = s_part_maps_per_char[c];
            map.clear();

            for (std::size_t cat_idx = 0; cat_idx < CATEGORY_COUNT; ++cat_idx)
            {
                // Effective Parts for character c = the per-character override if non-empty, else the base [Section]
                // Parts. Mirrors effective_parts_for_category() but parameterized on c instead of the active
                // character.
                const auto &override_parts = s_category_parts_per_char[cat_idx][c];
                const auto &effective = !override_parts.empty() ? override_parts : s_category_parts[cat_idx];
                if (effective.empty())
                    continue;
                parse_parts_into(
                    static_cast<Category>(cat_idx),
                    effective,
                    map,
                    /*diagnostics=*/false,
                    /*traces=*/false
                );
            }

            // Rebuild the per-character flat table from the freshly-populated map. Same flat-base / flat-end window as
            // the active-map path so the contiguous-range fast path is structurally identical.
            auto &flat = s_flat_tables_per_char[c];
            flat.fill(0);

            for (const auto &[hash, mask] : map)
            {
                if (hash >= FLAT_BASE && hash <= FLAT_END)
                    flat[hash - FLAT_BASE] = mask;
            }
        }
    }

    void rebuild_part_lookup()
    {
        std::lock_guard<std::mutex> lock(s_rebuild_mutex);
        invalidate_name_to_hash_map();
        s_part_maps[1 - s_active_map.load(std::memory_order_relaxed)].clear();

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto &parts = effective_parts_for_category(i);
            if (!parts.empty())
                register_parts(static_cast<Category>(i), parts, /*store_base=*/false);
        }

        build_part_lookup();
        // The per-character maps are an additive structure. The active-map double-buffer above serves callers that ask
        // only about the active character, while the per-character maps preserve swap-time identity at each vis ctrl.
        // A build here keeps both views in sync across every rebuild trigger (init, INI auto-reload,
        // set_active_character, deferred IndexedString scan completion).
        build_per_char_part_maps();

        // Per-category TRACE summary. Runs at the tail of every rebuild_part_lookup() caller (init post-scan,
        // deferred-scan converge, lazy-probe progress, set_active_character, INI auto-reload) so the reported counts
        // reflect the part map this call published.
        auto &logger = DMK::log();
        if (logger.get_log_level() <= DMK::LogLevel::Trace)
        {
            const auto &part_map = s_part_maps[s_active_map.load(std::memory_order_relaxed)];
            for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
            {
                const auto cat = static_cast<Category>(i);
                const auto bit = category_bit(cat);
                int count = 0;
                for (const auto &[hash, mask] : part_map)
                {
                    if (mask & bit)
                        ++count;
                }
                const bool enabled = s_states[i].enabled.load(std::memory_order_relaxed);
                const bool hidden = s_states[i].hidden.load(std::memory_order_relaxed);
                if (!enabled)
                    logger.trace("Category {}: disabled ({} parts registered)", category_section(cat), count);
                else
                    logger.trace(
                        "Category {}: enabled, default={} ({} parts)",
                        category_section(cat),
                        hidden ? "hidden" : "visible",
                        count
                    );
            }
        }
    }

    void set_active_character(int new_idx)
    {
        const int clamped = (new_idx < -1 || new_idx >= static_cast<int>(k_charIdxCount)) ? -1 : new_idx;
        const int prev = s_active_char.exchange(clamped, std::memory_order_acq_rel);
        if (prev == clamped)
            return;

        auto &logger = DMK::log();
        if (clamped >= 0)
            logger.info(
                "Active character -> {} (idx={})",
                character_name_for_idx(static_cast<std::size_t>(clamped)),
                clamped
            );
        else
            logger.info("Active character -> (base, no override)");

        rebuild_part_lookup();
    }

    CategoryMask classify_part(uint32_t hash) noexcept
    {
        const auto idx = s_active_map.load(std::memory_order_acquire);

        // Fast path: flat table lookup for the contiguous hash range
        if (hash >= FLAT_BASE && hash <= FLAT_END)
            return s_flat_tables[idx][hash - FLAT_BASE];

        // Slow path: binary search through sorted outlier array
        const auto &outliers = s_outlier_tables[idx];
        if (!outliers.empty())
        {
            auto it = std::lower_bound(
                outliers.begin(),
                outliers.end(),
                hash,
                [](const OutlierEntry &e, uint32_t h) { return e.hash < h; }
            );
            if (it != outliers.end() && it->hash == hash)
                return it->mask;
        }

        return 0;
    }

    bool needs_classification(uint32_t hash) noexcept
    {
        if (hash >= 0x10000)
            return false;
        const auto idx = s_active_map.load(std::memory_order_acquire);
        return (s_classify_bitsets[idx][hash / 64] & (1ULL << (hash % 64))) != 0;
    }

    bool is_category_hidden(Category cat) noexcept
    {
        const auto idx = static_cast<std::size_t>(cat);
        const auto &st = s_states[idx];
        return st.enabled.load(std::memory_order_relaxed) && st.hidden.load(std::memory_order_relaxed);
    }

    // Single atomic load replaces per-category iteration in the hot path.
    static std::atomic<CategoryMask> s_hidden_mask{0};
    static std::atomic<CategoryMask> s_preset_hidden_mask{0};
    static std::atomic<CategoryMask> s_active_preset_mask{0};

    void update_hidden_mask()
    {
        CategoryMask hidden = 0;
        CategoryMask preset_hidden = 0;
        CategoryMask active_preset = 0;

        for (std::size_t i = 0; i < CATEGORY_COUNT; ++i)
        {
            const auto &st = s_states[i];
            const bool enabled = st.enabled.load(std::memory_order_relaxed);
            const bool is_hidden = st.hidden.load(std::memory_order_relaxed);
            const auto bit = CategoryMask{1} << i;

            if (is_user_preset(static_cast<Category>(i)))
            {
                if (enabled)
                {
                    active_preset |= bit;
                    if (is_hidden)
                        preset_hidden |= bit;
                }
            }
            else
            {
                if (enabled && is_hidden)
                    hidden |= bit;
            }
        }

        s_active_preset_mask.store(active_preset, std::memory_order_relaxed);
        s_preset_hidden_mask.store(preset_hidden, std::memory_order_relaxed);
        s_hidden_mask.store(hidden, std::memory_order_relaxed);
    }

    bool is_any_category_hidden(CategoryMask mask) noexcept
    {
        const auto preset_overlap = mask & s_active_preset_mask.load(std::memory_order_relaxed);
        if (preset_overlap != 0)
            return (preset_overlap & s_preset_hidden_mask.load(std::memory_order_relaxed)) != 0;
        return (mask & s_hidden_mask.load(std::memory_order_relaxed)) != 0;
    }

    const std::unordered_map<uint32_t, CategoryMask> &get_part_map() noexcept
    {
        return s_part_maps[s_active_map.load(std::memory_order_acquire)];
    }

    CategoryMask classify_part_for(uint32_t hash, int char_idx) noexcept
    {
        if (char_idx < 0 || char_idx >= static_cast<int>(k_charIdxCount))
            return classify_part(hash);

        const auto c = static_cast<std::size_t>(char_idx);

        // Flat-table fast path for the contiguous range, mirroring the active-map classify_part hot path.
        if (hash >= FLAT_BASE && hash <= FLAT_END)
            return s_flat_tables_per_char[c][hash - FLAT_BASE];

        // Outliers: search the per-character unordered_map directly. A sorted outlier vector per character shaves a
        // few cycles off the rare miss but doubles the per-rebuild cost for a path that fires only on outlier hashes.
        const auto &map = s_part_maps_per_char[c];
        const auto it = map.find(hash);
        return (it != map.end()) ? it->second : CategoryMask{0};
    }

    const std::unordered_map<uint32_t, CategoryMask> &get_part_map_for(int char_idx) noexcept
    {
        // The out-of-range fallback returns the active-character map, so accidental misuse stays observably correct
        // instead of a crash.
        if (char_idx < 0 || char_idx >= static_cast<int>(k_charIdxCount))
            return get_part_map();
        return s_part_maps_per_char[static_cast<std::size_t>(char_idx)];
    }

    bool is_any_category_hidden_for(CategoryMask mask, int char_idx) noexcept
    {
        // The hidden-state masks (s_active_preset_mask, s_preset_hidden_mask, s_hidden_mask) are global: they
        // reflect the user's per-category Hidden / Enabled toggles, NOT the per-character Parts overrides. The split
        // happens in classify_part_for (different hashes -> different category bits), so the hidden-state computation
        // reuses the active-character helper verbatim once the caller has classified through the per-character map.
        (void)char_idx;
        return is_any_category_hidden(mask);
    }

} // namespace EquipHide
