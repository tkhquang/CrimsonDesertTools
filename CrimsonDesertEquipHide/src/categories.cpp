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
    // Game version: 1.01.00
    //
    // ---- Complete Part Hash Reference ----
    //
    // OneHandWeapons (0xAE03 - 0xAE1A, 0xB085 - 0xB086):
    //   0xAE03  CD_MainWeapon_Sword_R         Sword (Right, drawn)
    //   0xAE04  CD_MainWeapon_Sword_IN_R      Sword (Right, sheathed)
    //   0xAE05  CD_MainWeapon_Sword_L         Sword (Left, drawn)
    //   0xAE06  CD_MainWeapon_Sword_IN_L      Sword (Left, sheathed)
    //   0xAE07  CD_MainWeapon_Dagger_R        Dagger (Right, drawn)
    //   0xAE08  CD_MainWeapon_Dagger_IN_R     Dagger (Right, sheathed)
    //   0xAE09  CD_MainWeapon_Dagger_L        Dagger (Left, drawn)
    //   0xAE0A  CD_MainWeapon_Dagger_IN_L     Dagger (Left, sheathed)
    //   0xAE0B  CD_MainWeapon_Axe_R           Axe (Right)
    //   0xAE0C  CD_MainWeapon_Axe_L           Axe (Left)
    //   0xAE0D  CD_MainWeapon_Mace_R          Mace (Right)
    //   0xAE0E  CD_MainWeapon_Mace_L          Mace (Left)
    //   0xAE0F  CD_MainWeapon_Hammer_R        Hammer (Right)
    //   0xAE10  CD_MainWeapon_Flail_R         Flail (Right)
    //   0xAE11  CD_MainWeapon_Wand_R          Wand (Right)
    //   0xAE12  CD_MainWeapon_Bola            Bola
    //   0xAE13  CD_MainWeapon_Fist_R          Fist (Right)
    //   0xAE14  CD_MainWeapon_Fist_L          Fist (Left)
    //   0xAE15  CD_MainWeapon_HandCannon      Hand Cannon
    //   0xAE16  CD_MainWeapon_Fist_Hand       Fist (Hand)
    //   0xAE17  CD_MainWeapon_Fist_Foot       Fist (Foot)
    //   0xAE18  CD_MainWeapon_Lance           Lance
    //   0xAE19  CD_MainWeapon_Gauntlet        Gauntlet (Right)
    //   0xAE1A  CD_MainWeapon_Gauntlet_L      Gauntlet (Left)
    //   0xB085  CD_MainWeapon_Sword_R_Aux     Sword Aux (Right, drawn)
    //   0xB086  CD_MainWeapon_Sword_IN_R_Aux  Sword Aux (Right, sheathed)
    //
    // TwoHandWeapons (0xAE1B - 0xAE29, 0xAE41, 0xAF2A):
    //   0xAE1B  CD_TwoHandWeapon_Sword        Greatsword
    //   0xAE1C  CD_TwoHandWeapon_Axe          2H Axe
    //   0xAE1D  CD_TwoHandWeapon_Axe_Aux      2H Axe (Auxiliary)
    //   0xAE1E  CD_TwoHandWeapon_Mace         2H Mace
    //   0xAE1F  CD_TwoHandWeapon_WarHammer    War Hammer
    //   0xAE20  CD_TwoHandWeapon_Hammer       2H Hammer
    //   0xAE21  CD_TwoHandWeapon_Cannon       Cannon
    //   0xAE22  CD_TwoHandWeapon_CannonBall   Cannon Ball
    //   0xAE23  CD_TwoHandWeapon_Thrower      Thrower
    //   0xAE24  CD_TwoHandWeapon_Spear        Spear
    //   0xAE25  CD_TwoHandWeapon_Alebard      Halberd
    //   0xAE26  CD_MainWeapon_Pike            Pike
    //   0xAE27  CD_TwoHandWeapon_Rod          Rod
    //   0xAE28  CD_TwoHandWeapon_Flail        2H Flail
    //   0xAE29  CD_TwoHandWeapon_BlowPipe     Blow Pipe
    //   0xAE41  CD_TwoHandWeapon_Scythe       Scythe
    //   0xAF2A  CD_TwoHandWeapon_Flag         Flag
    //
    // Shields (0xAE2A - 0xAE2C):
    //   0xAE2A  CD_MainWeapon_Shield_L        Shield (Left)
    //   0xAE2B  CD_MainWeapon_Shield_R        Shield (Right)
    //   0xAE2C  CD_MainWeapon_TowerShield_L   Tower Shield (Left)
    //
    //   NOTE: Only 3 shield slot hashes exist in the IndexedStringA table.
    //   All shields (including unique/legendary like "Shield of Conviction")
    //   must use one of these slots. If a shield doesn't get hidden, it may
    //   render via an alternate VFX/overlay code path (see equip_hide.cpp).
    //
    // Bows (0xAE2D - 0xAE34, 0xAF28):
    //   0xAE2D  CD_MainWeapon_Bow             Bow
    //   0xAE2E  CD_MainWeapon_Quiver          Quiver
    //   0xAE2F  CD_MainWeapon_Quiver_Arw      Quiver Arrow (base)
    //   0xAE30  CD_MainWeapon_Quiver_Arw_01   Quiver Arrow 1
    //   0xAE31  CD_MainWeapon_Quiver_Arw_02   Quiver Arrow 2
    //   0xAE32  CD_MainWeapon_Quiver_Arw_03   Quiver Arrow 3
    //   0xAE33  CD_MainWeapon_Arw             Arrow
    //   0xAE34  CD_MainWeapon_Arwline         Arrow Line
    //   0xAF28  CD_MainWeapon_Arw_IN          Arrow (Sheathed)
    //
    // SpecialWeapons (0xAE35 - 0xAE3F):
    //   0xAE35  CD_MainWeapon_ArwHead         Arrow Head
    //   0xAE36  CD_MainWeapon_CrossBow        Crossbow
    //   0xAE37  CD_MainWeapon_Pistol_R        Pistol (Right)
    //   0xAE38  CD_MainWeapon_Pistol_L        Pistol (Left)
    //   0xAE39  CD_MainWeapon_Musket          Musket
    //   0xAE3A  CD_MainWeapon_Trap            Trap
    //   0xAE3B  CD_MainWeapon_Bomb            Bomb
    //   0xAE3C  CD_MainWeapon_Fan             Fan
    //   0xAE3D  CD_MainWeapon_ThrownSpear_R   Thrown Spear (Right)
    //   0xAE3E  CD_MainWeapon_ThrownSpear_L   Thrown Spear (Left)
    //   0xAE3F  CD_MainWeapon_Whip_R          Whip (Right)
    //
    // Tools (0x0F6D, 0xAE42 - 0xAE59, 0xAF2B, 0xAF2E, 0xAF6B, 0x12A79):
    //   0x0F6D  CD_Tool_FishingRod            Fishing Rod
    //   0xAE42  CD_Tool                       Tool (generic)
    //   0xAE43  CD_Tool_01                    Tool variant
    //   0xAE44  CD_Tool_02                    Tool variant
    //   0xAE45  CD_Tool_Axe                   Tool Axe
    //   0xAE46  CD_Tool_Hammer                Tool Hammer
    //   0xAE47  CD_Tool_Saw                   Tool Saw
    //   0xAE48  CD_Tool_Hoe                   Tool Hoe
    //   0xAE49  CD_Tool_Broom                 Tool Broom
    //   0xAE4A  CD_Tool_FarmScythe            Farm Scythe
    //   0xAE4B  CD_Tool_Hayfork               Hayfork
    //   0xAE4C  CD_Tool_Pickaxe               Pickaxe
    //   0xAE4D  CD_Tool_Rake                  Tool Rake
    //   0xAE4E  CD_Tool_Shovel                Tool Shovel
    //   0xAE4F  CD_Tool_Crutch                Tool Crutch
    //   0xAE50  CD_Tool_FishingRod_Sub        Fishing Rod (Sub)
    //   0xAE51  CD_Tool_Shooter               Shooter
    //   0xAE52  CD_Tool_Flute                 Flute
    //   0xAE53  CD_Tool_FireCan               Fire Can
    //   0xAE54  CD_Tool_Cigarette             Cigarette
    //   0xAE55  CD_Tool_Sprayer               Sprayer
    //   0xAE56  CD_Tool_HandDrum              Hand Drum
    //   0xAE57  CD_Tool_DrumStick_R           Drum Stick (Right)
    //   0xAE58  CD_Tool_DrumStick_L           Drum Stick (Left)
    //   0xAE59  CD_Tool_Torch                 Torch
    //   0xAF2B  CD_Tool_Pan                   Pan
    //   0xAF2E  CD_Tool_Trumpet               Trumpet
    //   0xAF6B  CD_Tool_Pipe                  Pipe
    // 0x12A79  CD_Tool_Book                  Book
    //
    // Lanterns (0xAE5A - 0xAE5C):
    //   0xAE5A  CD_Tool_Hyperspace_RemoteControl  Remote Control
    //   0xAE5B  CD_Lantern                        Lantern
    //   0xAE5C  CD_Lantern_Ring                   Lantern Ring
    //
    // NOT classified (system / accessories / mount / excluded):
    //   0xAD91  CD_Horse_Hair                 Horse Hair
    //   0xAD97  CD_Helm                       Helm
    //   0xAD98  CD_Helm_Acc                   Helm Accessory
    //   0xAD99  CD_Helm_Acc_01                Helm Accessory 1
    //   0xAD9A  CD_Helm_Acc_02                Helm Accessory 2
    //   0xAD9B  CD_Helm_Small                 Helm (Small)
    //   0xAD9C  CD_Helm_Visione_Belt          Helm Visione Belt
    //   0xADBE  CD_Bag                        Bag
    //   0xADBF  CD_Bag_Rocket                 Bag Rocket
    //   0xADC0  CD_Bag_For_Dock               Bag For Dock
    //   0xADC1  CD_Bag_Belt_For_Dock          Bag Belt For Dock
    //   0xADC3  CD_Bag_Small                  Bag Small
    //   0xADC4  CD_Bag_Acc                    Bag Accessory
    //   0xADC5  CD_Bag_Belt                   Bag Belt
    //   0xADC6  CD_Bag_Lantern                Bag Lantern
    //   0xADC7  CD_Bag_Rack                   Bag Rack
    //   0xADC8  CD_Ring_R                     Accessory (ring R)
    //   0xADC9  CD_Ring_L                     Accessory (ring L)
    //   0xADCA  CD_Glasses                    Accessory (glasses)
    //   0xADCB  CD_Earring_L                  Accessory (earring L)
    //   0xADCC  CD_Earring_R                  Accessory (earring R)
    //   0xADCD  CD_Necklace                   Accessory (necklace)
    //   0xADDF  CD_Abyss_Wing                 Abyss Wing
    //   0xADE0  CD_Abyss_Wing_01              Abyss Wing 1
    //   0xADE1  CD_Abyss_Wing_02              Abyss Wing 2
    //   0xADE2  CD_Abyss_Wing_03              Abyss Wing 3
    //   0xADE3  CD_Abyss_Glider               Abyss Glider
    //   0xADE4  CD_Abyss_Glider_01            Abyss Glider 1
    //   0xADE5  CD_Abyss_Glider_02            Abyss Glider 2
    //   0xADE6  CD_Abyss_WingSuit             Abyss WingSuit
    //   0xADE7  CD_Abyss_WingSuit_01          Abyss WingSuit 1
    //   0xADE8  CD_Abyss_WingSuit_02          Abyss WingSuit 2
    //   0xADED  CD_Wrist_BindingRope          Binding Rope
    //   0xADEE  CD_HyperspacePlug             Hyperspace Plug
    //   0xADEF  CD_AbyssGauntlet              Abyss Gauntlet
    //   0xADF0  CD_AbyssController            Abyss Controller
    //   0xADF1  CD_Abyss_Gauntlet             Abyss Gauntlet (variant)
    //   0xADF2  CD_Abyss_Gauntlet_01          Abyss Gauntlet 1
    //   0xADF3  CD_Abyss_Gauntlet_02          Abyss Gauntlet 2
    //   0xADF4  CD_Helm_Flight                Flight Helmet
    //   0xADF5  CD_Saddle                     Saddle
    //   0xADF6  CD_Saddle_Hook                Saddle Hook
    //   0xADF7  CD_Saddle_Belt                Saddle Belt
    //   0xADF8  CD_Armor_Halterbind           Armor Halterbind
    //   0xADF9  CD_Halterbind                 Halterbind
    //   0xADFB  CD_HorseShoe                  Horse Shoe
    //   0xADFC  CD_HorseHel                   Horse Helmet
    //   0xADFD  CD_HorseArmor                 Horse Armor
    //   0xADFE  CD_HorseArmor_01              Horse Armor 1
    //   0xADFF  CD_HorsePack                  Horse Pack
    //   0xAE00  CD_HorsePack_01               Horse Pack 1
    //   0xAE01  CD_Parachute                  Parachute
    //   0xAE02  CD_WagonWheel                 Wagon Wheel
    //   0xAE40  CD_PartHider                  System (never hide)
    //   0xAE5D  CD_Wagon_Lantern_R            Wagon Lantern R (excluded)
    //   0xAE5E  CD_Wagon_Lantern_L            Wagon Lantern L (excluded)
    //   0xAE5F  CD_Wagon_Lantern_Ring         Wagon Lantern Ring (excluded)
    //   0xAE60  CD_LandSpider_Shell           Land Spider Shell
    //   0xAE61  CD_LandSpider_Shell_01        Land Spider Shell 1
    //   0xAF2C  CD_MainWeapon_Parachute       Parachute (weapon slot)
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
        {"CD_MainWeapon_Sword_R",       0xAE03},
        {"CD_MainWeapon_Sword_IN_R",    0xAE04},
        {"CD_MainWeapon_Sword_L",       0xAE05},
        {"CD_MainWeapon_Sword_IN_L",    0xAE06},
        {"CD_MainWeapon_Dagger_R",      0xAE07},
        {"CD_MainWeapon_Dagger_IN_R",   0xAE08},
        {"CD_MainWeapon_Dagger_L",      0xAE09},
        {"CD_MainWeapon_Dagger_IN_L",   0xAE0A},
        {"CD_MainWeapon_Axe_R",         0xAE0B},
        {"CD_MainWeapon_Axe_L",         0xAE0C},
        {"CD_MainWeapon_Mace_R",        0xAE0D},
        {"CD_MainWeapon_Mace_L",        0xAE0E},
        {"CD_MainWeapon_Hammer_R",      0xAE0F},
        {"CD_MainWeapon_Flail_R",       0xAE10},
        {"CD_MainWeapon_Wand_R",        0xAE11},
        {"CD_MainWeapon_Bola",          0xAE12},
        {"CD_MainWeapon_Fist_R",        0xAE13},
        {"CD_MainWeapon_Fist_L",        0xAE14},
        {"CD_MainWeapon_HandCannon",    0xAE15},
        {"CD_MainWeapon_Fist_Hand",     0xAE16},
        {"CD_MainWeapon_Fist_Foot",     0xAE17},
        {"CD_MainWeapon_Lance",         0xAE18},
        {"CD_MainWeapon_Gauntlet",      0xAE19},
        {"CD_MainWeapon_Gauntlet_L",    0xAE1A},
        {"CD_MainWeapon_Sword_R_Aux",   0xB085},
        {"CD_MainWeapon_Sword_IN_R_Aux",0xB086},
        // 2H Weapons
        {"CD_TwoHandWeapon_Sword",      0xAE1B},
        {"CD_TwoHandWeapon_Axe",        0xAE1C},
        {"CD_TwoHandWeapon_Axe_Aux",    0xAE1D},
        {"CD_TwoHandWeapon_Mace",       0xAE1E},
        {"CD_TwoHandWeapon_WarHammer",  0xAE1F},
        {"CD_TwoHandWeapon_Hammer",     0xAE20},
        {"CD_TwoHandWeapon_Cannon",     0xAE21},
        {"CD_TwoHandWeapon_CannonBall", 0xAE22},
        {"CD_TwoHandWeapon_Thrower",    0xAE23},
        {"CD_TwoHandWeapon_Spear",      0xAE24},
        {"CD_TwoHandWeapon_Alebard",    0xAE25},
        {"CD_MainWeapon_Pike",          0xAE26},
        {"CD_TwoHandWeapon_Rod",        0xAE27},
        {"CD_TwoHandWeapon_Flail",      0xAE28},
        {"CD_TwoHandWeapon_BlowPipe",   0xAE29},
        {"CD_TwoHandWeapon_Scythe",     0xAE41},
        {"CD_TwoHandWeapon_Flag",       0xAF2A},
        // Shields
        {"CD_MainWeapon_Shield_L",      0xAE2A},
        {"CD_MainWeapon_Shield_R",      0xAE2B},
        {"CD_MainWeapon_TowerShield_L", 0xAE2C},
        // Bows / Arrows
        {"CD_MainWeapon_Bow",           0xAE2D},
        {"CD_MainWeapon_Quiver",        0xAE2E},
        {"CD_MainWeapon_Quiver_Arw",    0xAE2F},
        {"CD_MainWeapon_Quiver_Arw_01", 0xAE30},
        {"CD_MainWeapon_Quiver_Arw_02", 0xAE31},
        {"CD_MainWeapon_Quiver_Arw_03", 0xAE32},
        {"CD_MainWeapon_Arw",           0xAE33},
        {"CD_MainWeapon_Arwline",       0xAE34},
        {"CD_MainWeapon_Arw_IN",        0xAF28},
        // Special / Ranged
        {"CD_MainWeapon_ArwHead",       0xAE35},
        {"CD_MainWeapon_CrossBow",      0xAE36},
        {"CD_MainWeapon_Pistol_R",      0xAE37},
        {"CD_MainWeapon_Pistol_L",      0xAE38},
        {"CD_MainWeapon_Musket",        0xAE39},
        {"CD_MainWeapon_Trap",          0xAE3A},
        {"CD_MainWeapon_Bomb",          0xAE3B},
        {"CD_MainWeapon_Fan",           0xAE3C},
        {"CD_MainWeapon_ThrownSpear_R", 0xAE3D},
        {"CD_MainWeapon_ThrownSpear_L", 0xAE3E},
        {"CD_MainWeapon_Whip_R",        0xAE3F},
        // Tools
        {"CD_Tool_FishingRod",          0x0F6D},
        {"CD_Tool",                     0xAE42},
        {"CD_Tool_01",                  0xAE43},
        {"CD_Tool_02",                  0xAE44},
        {"CD_Tool_Axe",                 0xAE45},
        {"CD_Tool_Hammer",              0xAE46},
        {"CD_Tool_Saw",                 0xAE47},
        {"CD_Tool_Hoe",                 0xAE48},
        {"CD_Tool_Broom",               0xAE49},
        {"CD_Tool_FarmScythe",          0xAE4A},
        {"CD_Tool_Hayfork",             0xAE4B},
        {"CD_Tool_Pickaxe",             0xAE4C},
        {"CD_Tool_Rake",                0xAE4D},
        {"CD_Tool_Shovel",              0xAE4E},
        {"CD_Tool_Crutch",              0xAE4F},
        {"CD_Tool_FishingRod_Sub",      0xAE50},
        {"CD_Tool_Shooter",             0xAE51},
        {"CD_Tool_Flute",               0xAE52},
        {"CD_Tool_FireCan",             0xAE53},
        {"CD_Tool_Cigarette",           0xAE54},
        {"CD_Tool_Sprayer",             0xAE55},
        {"CD_Tool_HandDrum",            0xAE56},
        {"CD_Tool_DrumStick_R",         0xAE57},
        {"CD_Tool_DrumStick_L",         0xAE58},
        {"CD_Tool_Torch",               0xAE59},
        {"CD_Tool_Pan",                 0xAF2B},
        {"CD_Tool_Trumpet",             0xAF2E},
        {"CD_Tool_Pipe",                0xAF6B},
        {"CD_Tool_Book",                0x12A79},
        // Lanterns
        {"CD_Tool_Hyperspace_RemoteControl", 0xAE5A},
        {"CD_Lantern",                  0xAE5B},
        {"CD_Lantern_Ring",             0xAE5C},
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
    // Parts parser + lookup map
    //
    // Accepts comma-separated part names (e.g. "CD_Tool_Torch, CD_Lantern").
    // Also supports raw hex IDs (e.g. "0xAE1D") for advanced users.
    // =========================================================================
    static std::unordered_map<uint32_t, Category> s_partMap;

    void register_parts(Category cat, const std::string& partsStr)
    {
        auto& logger = DMK::Logger::get_instance();

        if (partsStr.find_first_not_of(" ,") == std::string::npos)
        {
            if (!partsStr.empty())
                logger.warning("{}: no parts configured (check INI file)", category_section(cat));
            return;
        }

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

    const std::unordered_map<uint32_t, Category>& get_part_map()
    {
        return s_partMap;
    }

} // namespace EquipHide
