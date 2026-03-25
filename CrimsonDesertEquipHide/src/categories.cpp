#include "categories.hpp"

#include <array>
#include <cstdint>

namespace EquipHide
{
    // =========================================================================
    // Singleton state array
    // =========================================================================
    static std::array<CategoryState, CATEGORY_COUNT> s_states{};

    std::array<CategoryState, CATEGORY_COUNT>& category_states()
    {
        return s_states;
    }

    // =========================================================================
    // Hash-range classification
    //
    // Part hashes are IndexedStringA IDs read from *(DWORD*)R10 at hook point.
    // Ranges verified via CE runtime breakpoints + string table reads.
    // See .idea/research/equip_hide_v2.md for full mapping.
    //
    // Game version: 1.00.03
    //
    // ---- Complete Part Hash Reference ----
    //
    // OneHandWeapons (0xADC7 - 0xADDE):
    //   0xADC7  CD_MainWeapon_Sword_R         Sword (Right, drawn)
    //   0xADC8  CD_MainWeapon_Sword_IN_R      Sword (Right, sheathed)
    //   0xADC9  CD_MainWeapon_Sword_L         Sword (Left, drawn)
    //   0xADCA  CD_MainWeapon_Sword_IN_L      Sword (Left, sheathed)
    //   0xADCB  CD_MainWeapon_Dagger_R        Dagger (Right, drawn)
    //   0xADCC  CD_MainWeapon_Dagger_IN_R     Dagger (Right, sheathed)
    //   0xADCD  CD_MainWeapon_Dagger_L        Dagger (Left, drawn)
    //   0xADCE  CD_MainWeapon_Dagger_IN_L     Dagger (Left, sheathed)
    //   0xADCF  CD_MainWeapon_Axe_R           Axe (Right)
    //   0xADD0  CD_MainWeapon_Axe_L           Axe (Left)
    //   0xADD1  CD_MainWeapon_Mace_R          Mace (Right)
    //   0xADD2  CD_MainWeapon_Mace_L          Mace (Left)
    //   0xADD3  CD_MainWeapon_Hammer_R        Hammer (Right)
    //   0xADD4  CD_MainWeapon_Flail_R         Flail (Right)
    //   0xADD5  CD_MainWeapon_Wand_R          Wand (Right)
    //   0xADD6  CD_MainWeapon_Bola            Bola
    //   0xADD7  CD_MainWeapon_Fist_R          Fist (Right)
    //   0xADD8  CD_MainWeapon_Fist_L          Fist (Left)
    //   0xADD9  CD_MainWeapon_HandCannon      Hand Cannon
    //   0xADDA  CD_MainWeapon_Fist_Hand       Fist (Hand)
    //   0xADDB  CD_MainWeapon_Fist_Foot       Fist (Foot)
    //   0xADDC  CD_MainWeapon_Lance           Lance
    //   0xADDD  CD_MainWeapon_Gauntlet        Gauntlet (Right)
    //   0xADDE  CD_MainWeapon_Gauntlet_L      Gauntlet (Left)
    //
    // TwoHandWeapons (0xADDF - 0xADED, 0xAE05):
    //   0xADDF  CD_TwoHandWeapon_Sword        Greatsword
    //   0xADE0  CD_TwoHandWeapon_Axe          2H Axe
    //   0xADE1  CD_TwoHandWeapon_Axe_Aux      2H Axe (Auxiliary)
    //   0xADE2  CD_TwoHandWeapon_Mace         2H Mace
    //   0xADE3  CD_TwoHandWeapon_WarHammer    War Hammer
    //   0xADE4  CD_TwoHandWeapon_Hammer       2H Hammer
    //   0xADE5  CD_TwoHandWeapon_Cannon       Cannon
    //   0xADE6  CD_TwoHandWeapon_CannonBall   Cannon Ball
    //   0xADE7  CD_TwoHandWeapon_Thrower      Thrower
    //   0xADE8  CD_TwoHandWeapon_Spear        Spear
    //   0xADE9  CD_TwoHandWeapon_Alebard      Halberd
    //   0xADEA  CD_MainWeapon_Pike            Pike
    //   0xADEB  CD_TwoHandWeapon_Rod          Rod
    //   0xADEC  CD_TwoHandWeapon_Flail        2H Flail
    //   0xADED  CD_TwoHandWeapon_BlowPipe     Blow Pipe
    //   0xAE05  CD_TwoHandWeapon_Scythe       Scythe
    //
    // Shields (0xADEE - 0xADF0):
    //   0xADEE  CD_MainWeapon_Shield_L        Shield (Left)
    //   0xADEF  CD_MainWeapon_Shield_R        Shield (Right)
    //   0xADF0  CD_MainWeapon_TowerShield_L   Tower Shield (Left)
    //
    // Bows (0xADF1 - 0xADF8):
    //   0xADF1  CD_MainWeapon_Bow             Bow
    //   0xADF2  CD_MainWeapon_Quiver          Quiver
    //   0xADF4  CD_MainWeapon_Quiver_Arw_01   Quiver Arrow 1
    //   0xADF5  CD_MainWeapon_Quiver_Arw_02   Quiver Arrow 2
    //   0xADF6  CD_MainWeapon_Quiver_Arw_03   Quiver Arrow 3
    //   0xADF7  CD_MainWeapon_Arw             Arrow
    //   0xADF8  CD_MainWeapon_Arwline         Arrow Line
    //
    // SpecialWeapons (0xADF9 - 0xAE02):
    //   0xADF9  CD_MainWeapon_ArwHead         Arrow Head
    //   0xADFA  CD_MainWeapon_CrossBow        Crossbow
    //   0xADFB  CD_MainWeapon_Pistol_R        Pistol (Right)
    //   0xADFC  CD_MainWeapon_Pistol_L        Pistol (Left)
    //   0xADFD  CD_MainWeapon_Musket          Musket
    //   0xADFE  CD_MainWeapon_Trap            Trap
    //   0xADFF  CD_MainWeapon_Bomb            Bomb
    //   0xAE00  CD_MainWeapon_Fan             Fan
    //   0xAE01  CD_MainWeapon_ThrownSpear_R   Thrown Spear (Right)
    //   0xAE02  CD_MainWeapon_ThrownSpear_L   Thrown Spear (Left)
    //
    // Tools (0xAE06 - 0xAE1D, 0x0F4F):
    //   0x0F4F  CD_Tool_FishingRod_Sub        Fishing Rod
    //   0xAE06  CD_Tool                       Tool (generic)
    //   0xAE07  CD_Tool_01                    Tool variant
    //   0xAE08  CD_Tool_02                    Tool variant
    //   0xAE09  CD_Tool_Axe                   Tool Axe
    //   0xAE0A  CD_Tool_Hammer                Tool Hammer
    //   0xAE0B  CD_Tool_Saw                   Tool Saw
    //   0xAE0C  CD_Tool_Hoe                   Tool Hoe
    //   0xAE0D  CD_Tool_Broom                 Tool Broom
    //   0xAE0E  CD_Tool_FarmScythe            Farm Scythe
    //   0xAE11  CD_Tool_Rake                  Tool Rake
    //   0xAE12  CD_Tool_Shovel                Tool Shovel
    //   0xAE13  CD_Tool_Crutch                Tool Crutch
    //   0xAE16  CD_Tool_Flute                 Flute
    //   0xAE18  CD_Tool_Cigarette             Cigarette
    //   0xAE1A  CD_Tool_HandDrum              Hand Drum
    //   0xAE1B  CD_Tool_DrumStick_R           Drum Stick (Right)
    //   0xAE1C  CD_Tool_DrumStick_L           Drum Stick (Left)
    //   0xAE1D  CD_Tool_Torch                 Torch
    //   (0xAE0F, 0xAE10, 0xAE14, 0xAE15, 0xAE17, 0xAE19 = gap IDs, no string entry)
    //
    // Lanterns (0xAE1E - 0xAE20):
    //   0xAE1F  CD_Lantern                    Lantern
    //   (0xAE1E, 0xAE20 = gap IDs seen at runtime, no string entry)
    //
    // NOT classified (system / NPC / excluded):
    //   0xAD8D  CD_Ring_L                     Accessory (ring)
    //   0xAD8E  CD_Glasses                    Accessory (glasses)
    //   0xAD90  CD_Earring_R                  Accessory (earring)
    //   0xAD91  CD_Necklace                   Accessory (necklace)
    //   0xADB3  CD_AbyssGauntlet              Abyss Gauntlet
    //   0xADB8  CD_Helm_Flight                Flight Helmet
    //   0xAE04  CD_PartHider                  System (never hide)
    //   0xAE21  CD_Wagon_Lantern_R            Wagon Lantern R (excluded)
    //   0xAE22  CD_Wagon_Lantern_L            Wagon Lantern L (excluded)
    //   0xAE23  CD_Wagon_Lantern_Ring         Wagon Lantern Ring (excluded)
    //   0xAE24  CD_LandSpider_Shell           Land Spider Shell (excluded)
    //   0xAExx  (dynamic)                     NPC/mount slots
    //   0xAFxx  (dynamic)                     NPC/mount slots
    //   0xB0xx  (dynamic)                     NPC/mount slots
    //
    // =========================================================================
    // =========================================================================
    // Name → hash table (all known parts)
    // =========================================================================
    struct NamedPart
    {
        const char* name;
        uint32_t    hash;
    };

    // clang-format off
    static constexpr NamedPart k_allParts[] = {
        // 1H Weapons
        {"CD_MainWeapon_Sword_R",       0xADC7},
        {"CD_MainWeapon_Sword_IN_R",    0xADC8},
        {"CD_MainWeapon_Sword_L",       0xADC9},
        {"CD_MainWeapon_Sword_IN_L",    0xADCA},
        {"CD_MainWeapon_Dagger_R",      0xADCB},
        {"CD_MainWeapon_Dagger_IN_R",   0xADCC},
        {"CD_MainWeapon_Dagger_L",      0xADCD},
        {"CD_MainWeapon_Dagger_IN_L",   0xADCE},
        {"CD_MainWeapon_Axe_R",         0xADCF},
        {"CD_MainWeapon_Axe_L",         0xADD0},
        {"CD_MainWeapon_Mace_R",        0xADD1},
        {"CD_MainWeapon_Mace_L",        0xADD2},
        {"CD_MainWeapon_Hammer_R",      0xADD3},
        {"CD_MainWeapon_Flail_R",       0xADD4},
        {"CD_MainWeapon_Wand_R",        0xADD5},
        {"CD_MainWeapon_Bola",          0xADD6},
        {"CD_MainWeapon_Fist_R",        0xADD7},
        {"CD_MainWeapon_Fist_L",        0xADD8},
        {"CD_MainWeapon_HandCannon",    0xADD9},
        {"CD_MainWeapon_Fist_Hand",     0xADDA},
        {"CD_MainWeapon_Fist_Foot",     0xADDB},
        {"CD_MainWeapon_Lance",         0xADDC},
        {"CD_MainWeapon_Gauntlet",      0xADDD},
        {"CD_MainWeapon_Gauntlet_L",    0xADDE},
        // 2H Weapons
        {"CD_TwoHandWeapon_Sword",      0xADDF},
        {"CD_TwoHandWeapon_Axe",        0xADE0},
        {"CD_TwoHandWeapon_Axe_Aux",    0xADE1},
        {"CD_TwoHandWeapon_Mace",       0xADE2},
        {"CD_TwoHandWeapon_WarHammer",  0xADE3},
        {"CD_TwoHandWeapon_Hammer",     0xADE4},
        {"CD_TwoHandWeapon_Cannon",     0xADE5},
        {"CD_TwoHandWeapon_CannonBall", 0xADE6},
        {"CD_TwoHandWeapon_Thrower",    0xADE7},
        {"CD_TwoHandWeapon_Spear",      0xADE8},
        {"CD_TwoHandWeapon_Alebard",    0xADE9},
        {"CD_MainWeapon_Pike",          0xADEA},
        {"CD_TwoHandWeapon_Rod",        0xADEB},
        {"CD_TwoHandWeapon_Flail",      0xADEC},
        {"CD_TwoHandWeapon_BlowPipe",   0xADED},
        {"CD_TwoHandWeapon_Scythe",     0xAE05},
        // Shields
        {"CD_MainWeapon_Shield_L",      0xADEE},
        {"CD_MainWeapon_Shield_R",      0xADEF},
        {"CD_MainWeapon_TowerShield_L", 0xADF0},
        // Bows / Arrows
        {"CD_MainWeapon_Bow",           0xADF1},
        {"CD_MainWeapon_Quiver",        0xADF2},
        {"CD_MainWeapon_Quiver_Arw_01", 0xADF4},
        {"CD_MainWeapon_Quiver_Arw_02", 0xADF5},
        {"CD_MainWeapon_Quiver_Arw_03", 0xADF6},
        {"CD_MainWeapon_Arw",           0xADF7},
        {"CD_MainWeapon_Arwline",       0xADF8},
        // Special / Ranged
        {"CD_MainWeapon_ArwHead",       0xADF9},
        {"CD_MainWeapon_CrossBow",      0xADFA},
        {"CD_MainWeapon_Pistol_R",      0xADFB},
        {"CD_MainWeapon_Pistol_L",      0xADFC},
        {"CD_MainWeapon_Musket",        0xADFD},
        {"CD_MainWeapon_Trap",          0xADFE},
        {"CD_MainWeapon_Bomb",          0xADFF},
        {"CD_MainWeapon_Fan",           0xAE00},
        {"CD_MainWeapon_ThrownSpear_R", 0xAE01},
        {"CD_MainWeapon_ThrownSpear_L", 0xAE02},
        // Tools
        {"CD_Tool",                     0xAE06},
        {"CD_Tool_01",                  0xAE07},
        {"CD_Tool_02",                  0xAE08},
        {"CD_Tool_Axe",                 0xAE09},
        {"CD_Tool_Hammer",              0xAE0A},
        {"CD_Tool_Saw",                 0xAE0B},
        {"CD_Tool_Hoe",                 0xAE0C},
        {"CD_Tool_Broom",              0xAE0D},
        {"CD_Tool_FarmScythe",          0xAE0E},
        {"CD_Tool_Rake",                0xAE11},
        {"CD_Tool_Shovel",              0xAE12},
        {"CD_Tool_Crutch",              0xAE13},
        {"CD_Tool_Flute",               0xAE16},
        {"CD_Tool_Cigarette",           0xAE18},
        {"CD_Tool_HandDrum",            0xAE1A},
        {"CD_Tool_DrumStick_R",         0xAE1B},
        {"CD_Tool_DrumStick_L",         0xAE1C},
        {"CD_Tool_Torch",               0xAE1D},
        {"CD_Tool_FishingRod_Sub",      0x0F4F},
        // Lanterns
        {"CD_Lantern",                  0xAE1F},
    };
    // clang-format on

    /// Build name→hash lookup (lazy init, populated on first use)
    static const std::unordered_map<std::string, uint32_t>& name_to_hash_map()
    {
        static std::unordered_map<std::string, uint32_t> map;
        if (map.empty())
        {
            for (const auto& p : k_allParts)
                map[p.name] = p.hash;
        }
        return map;
    }

    // =========================================================================
    // Default parts strings (human-readable names, used when INI omits "Parts")
    // =========================================================================
    // clang-format off
    static constexpr std::string_view k_defaultParts[] = {
        // OneHandWeapons
        "CD_MainWeapon_Sword_R, CD_MainWeapon_Sword_IN_R, CD_MainWeapon_Sword_L, CD_MainWeapon_Sword_IN_L, "
        "CD_MainWeapon_Dagger_R, CD_MainWeapon_Dagger_IN_R, CD_MainWeapon_Dagger_L, CD_MainWeapon_Dagger_IN_L, "
        "CD_MainWeapon_Axe_R, CD_MainWeapon_Axe_L, CD_MainWeapon_Mace_R, CD_MainWeapon_Mace_L, "
        "CD_MainWeapon_Hammer_R, CD_MainWeapon_Flail_R, CD_MainWeapon_Wand_R, CD_MainWeapon_Bola, "
        "CD_MainWeapon_Fist_R, CD_MainWeapon_Fist_L, CD_MainWeapon_HandCannon, "
        "CD_MainWeapon_Fist_Hand, CD_MainWeapon_Fist_Foot, CD_MainWeapon_Lance, "
        "CD_MainWeapon_Gauntlet, CD_MainWeapon_Gauntlet_L",

        // TwoHandWeapons
        "CD_TwoHandWeapon_Sword, CD_TwoHandWeapon_Axe, CD_TwoHandWeapon_Axe_Aux, "
        "CD_TwoHandWeapon_Mace, CD_TwoHandWeapon_WarHammer, CD_TwoHandWeapon_Hammer, "
        "CD_TwoHandWeapon_Cannon, CD_TwoHandWeapon_CannonBall, CD_TwoHandWeapon_Thrower, "
        "CD_TwoHandWeapon_Spear, CD_TwoHandWeapon_Alebard, CD_MainWeapon_Pike, "
        "CD_TwoHandWeapon_Rod, CD_TwoHandWeapon_Flail, CD_TwoHandWeapon_BlowPipe, "
        "CD_TwoHandWeapon_Scythe",

        // Shields
        "CD_MainWeapon_Shield_L, CD_MainWeapon_Shield_R, CD_MainWeapon_TowerShield_L",

        // Bows (0xADF3 = gap ID seen at runtime)
        "CD_MainWeapon_Bow, CD_MainWeapon_Quiver, "
        "CD_MainWeapon_Quiver_Arw_01, CD_MainWeapon_Quiver_Arw_02, CD_MainWeapon_Quiver_Arw_03, "
        "CD_MainWeapon_Arw, CD_MainWeapon_Arwline, 0xADF3",

        // SpecialWeapons
        "CD_MainWeapon_ArwHead, CD_MainWeapon_CrossBow, "
        "CD_MainWeapon_Pistol_R, CD_MainWeapon_Pistol_L, CD_MainWeapon_Musket, "
        "CD_MainWeapon_Trap, CD_MainWeapon_Bomb, CD_MainWeapon_Fan, "
        "CD_MainWeapon_ThrownSpear_R, CD_MainWeapon_ThrownSpear_L",

        // Tools (gap IDs at end: 0xAE0F, 0xAE10, 0xAE14, 0xAE15, 0xAE17, 0xAE19)
        "CD_Tool, CD_Tool_01, CD_Tool_02, CD_Tool_Axe, CD_Tool_Hammer, CD_Tool_Saw, "
        "CD_Tool_Hoe, CD_Tool_Broom, CD_Tool_FarmScythe, CD_Tool_Rake, CD_Tool_Shovel, "
        "CD_Tool_Crutch, CD_Tool_Flute, CD_Tool_Cigarette, CD_Tool_HandDrum, "
        "CD_Tool_DrumStick_R, CD_Tool_DrumStick_L, CD_Tool_Torch, CD_Tool_FishingRod_Sub, "
        "0xAE0F, 0xAE10, 0xAE14, 0xAE15, 0xAE17, 0xAE19",

        // Lanterns (gap IDs at end: 0xAE1E, 0xAE20)
        "CD_Lantern, 0xAE1E, 0xAE20",
    };
    // clang-format on

    std::string_view default_parts(Category cat)
    {
        return k_defaultParts[static_cast<std::size_t>(cat)];
    }

    // =========================================================================
    // Parts parser + lookup map
    //
    // Accepts comma-separated part names (e.g. "CD_Tool_Torch, CD_Lantern").
    // Also supports raw hex IDs (e.g. "0xAE1D") for advanced users.
    // =========================================================================
    static std::unordered_map<uint32_t, Category> s_partMap;

    void register_parts(Category cat, const std::string& partsStr)
    {
        auto& logger = DMK::Logger::get_instance();
        const auto& nameMap = name_to_hash_map();

        std::size_t pos = 0;
        while (pos < partsStr.size())
        {
            // Skip whitespace and commas
            while (pos < partsStr.size() && (partsStr[pos] == ' ' || partsStr[pos] == ','))
                ++pos;
            if (pos >= partsStr.size())
                break;

            // Read token until comma or end
            auto end = partsStr.find(',', pos);
            if (end == std::string::npos)
                end = partsStr.size();

            std::string token = partsStr.substr(pos, end - pos);
            pos = end;

            // Trim whitespace
            while (!token.empty() && token.back() == ' ')
                token.pop_back();
            while (!token.empty() && token.front() == ' ')
                token.erase(token.begin());

            if (token.empty())
                continue;

            // Try as part name first
            auto it = nameMap.find(token);
            if (it != nameMap.end())
            {
                s_partMap[it->second] = cat;
                logger.debug("  {} += {} (0x{:04X})", category_section(cat), token, it->second);
                continue;
            }

            // Try as hex ID (for advanced users / gap IDs)
            if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
            {
                try
                {
                    auto id = static_cast<uint32_t>(std::stoul(token.substr(2), nullptr, 16));
                    s_partMap[id] = cat;
                    logger.debug("  {} += 0x{:04X} (raw)", category_section(cat), id);
                    continue;
                }
                catch (...) {}
            }

            logger.warning("  {} : unknown part '{}'", category_section(cat), token);
        }
    }

    void build_part_lookup()
    {
        DMK::Logger::get_instance().info("Part lookup built: {} entries across {} categories",
                                          s_partMap.size(), CATEGORY_COUNT);
    }

    std::optional<Category> classify_part(uint32_t hash)
    {
        const auto it = s_partMap.find(hash);
        if (it != s_partMap.end())
            return it->second;
        return std::nullopt;
    }

    bool is_category_hidden(Category cat)
    {
        const auto idx = static_cast<std::size_t>(cat);
        const auto& st = s_states[idx];
        return st.enabled.load(std::memory_order_relaxed) &&
               st.hidden.load(std::memory_order_relaxed);
    }

} // namespace EquipHide
