#pragma once

#include "shared_state.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Transmog
{
    // Per-(character, slot) default carrier item used by LT's transmog
    // apply path AND by the prefab-wrapper-swap picker.
    //
    //   itemName   -- resolved through ItemNameTable::id_of() at runtime
    //                 to produce a uint16_t carrier itemId. Drives
    //                 SlotPopulator(itemId) for the carrier-equip path.
    //   prefabName -- the body-mesh prefab the carrier emits when
    //                 equipped (e.g. "cd_phm_02_sword_0015"). Used by
    //                 PWS::populate_slot_catalogs as the picker's
    //                 default src selection so the swap-map's source
    //                 wrapper matches the carrier's natural emit. Empty
    //                 string when no body-mesh swap is configured for
    //                 this slot/character (the carrier still works as
    //                 a plain itemId; just no prefab-level intercept).
    //
    // Both fields refer to the SAME logical default item per (char, slot).
    // Centralising them here prevents the drift class where one was
    // updated and the other forgotten (e.g. the spear-carrier test that
    // failed because PWS src still pointed at sword_0015).
    struct CarrierDefault
    {
        const char *itemName;
        const char *prefabName;
    };

    // Character axis. Order is fixed; CarrierChar(i) maps to
    // k_carriers[i]. New characters append at the end before Count.
    enum class CarrierChar : std::size_t
    {
        Kliff = 0,
        Damiane = 1,
        Oongka = 2,
        Count
    };

    inline constexpr std::size_t k_carrierCharCount =
        static_cast<std::size_t>(CarrierChar::Count);

    // 2D table indexed [character][slot]. Adding a slot = one column
    // in each character row. Adding a character = one new row.
    // clang-format off
    inline constexpr CarrierDefault k_carriers[k_carrierCharCount][k_slotCount] = {
        // ============================================================
        // Kliff (male). Default carriers per slot from the live
        // slot-discovery dump; prefabName values from the in-game
        // item-mesh mapping.
        // ============================================================
        {
            { "Scovi_Fabric_Helm",                       "cd_phm_00_hel_0122"                }, // Helm
            { "Mercenary_Leather_Armor",                      "cd_phm_00_ub_00_0054"              }, // Chest
            { "Mercenary_Leather_Cloak",                      "cd_phm_00_cloak_00_0054_s"         }, // Cloak
            { "Mercenary_Gloves",                     "cd_phm_00_hand_0054"               }, // Gloves
            { "Mercenary_Leather_Boots",                           "cd_phm_00_foot_0054"               }, // Boots
            { "Hexe_Earring",                                "cd_phm_00_earring_0013_l"          }, // Earring1
            { "Ancient_People_Earring",                      "cd_phm_00_earring_0004_r"          }, // Earring2
            { "Titan_Necklace",                              "cd_phm_00_necklace_0017"           }, // Necklace
            { "Antumbra_DarkDeacon_Ring",                    "cd_phm_00_ring_0018_l"             }, // Ring1
            { "AbyssReward_EastWitch_Ring",                  "cd_phm_00_ring_0017_r"             }, // Ring2 (variants: _l/_r)
            { "RedNose_Lantern",                             "cd_t0000_lantern_0003"             }, // Lantern
            { "Kliff_Glasses",                               "cd_phm_00_glasses_0002"            }, // Glasses
            { "Kliff_Mask",                                  "cd_phm_00_mask_00_0271_a"          }, // Mask
            { "Aggro_Backpack",                              "cd_phm_00_bag_0053"                }, // Backpack
            { "Daeil_Band",                                  "cd_phm_00_rinkband_0001"           }, // Bracelet (phm, male rig)
            { "Legendary_Dragon_OneHandSword",               "cd_phm_01_sword_0023_r"            }, // MainHand
            { "Legendary_Dragon_OneHandSword",               "cd_phm_01_sword_0023_l"            }, // OffHand (mirror)
            { "GreyWolf_OneHandBow",                         "cd_phm_04_bow_0003"                }, // Ranged
            { "Legendary_Shakatu_OneHandDagger",             "cd_phm_01_dagger_0079_r"           }, // SubWeapon
            { "Legendary_Antumbra_TwoHandGiantBastard",      "cd_phm_02_sword_0015"              }, // TwoHandWeapon
        },

        // ============================================================
        // Damiane (female). Demeniss Elite/Uniform Leather armor set +
        // Pattern jewelry set + Damian_OneHandPistol Ranged. prefab
        // values derived from `00_idea/items_to_prefabs_v105_01.tsv`.
        // Engine uses cd_phw_* for female-specific assets and cd_phm_*
        // for shared accessories. Some armor pieces use _index##
        // variants -- LT's prefab-wrapper hook registers all variants
        // of the base prefab so any one variant works as the picker
        // default seed.
        // ============================================================
        {
            { "Demian_PlateArmor_Helm_VII",                  "cd_phw_00_hel_00_0162"             }, // Helm
            { "Damian_Demeniss_Elite_Uniform_Leather_Armor", "cd_phw_00_ub_00_0163_index01"      }, // Chest
            { "Damian_Demeniss_Uniform_Leather_Cloak",       "cd_phw_00_cloak_00_0163_index01_t" }, // Cloak
            { "Damian_Demeniss_Uniform_Leather_Gloves",      "cd_phw_01_hand_00_0380_index03"    }, // Gloves
            { "Damian_Demeniss_Elite_Uniform_Leather_Boots", "cd_phw_00_foot_00_0163_index01"    }, // Boots
            { "Pattern_Bronze_Earring",                      "cd_phm_00_earring_0017_l"          }, // Earring1
            { "Pattern_Silver_Earring",                      "cd_phm_00_earring_0019_r"          }, // Earring2
            { "Pattern_Copper_Necklace",                     "cd_phm_00_necklace_0026"           }, // Necklace
            { "Hernand_Nobility_Degree_I",                   "cd_phm_00_ring_0006_r"             }, // Ring1
            { "Hernand_Nobility_Degree_I",                   "cd_phm_00_ring_0006_r"             }, // Ring2 (same item in both rings)
            { "Lantern",                                     ""                                  }, // Lantern (cd_t0000_* family, not in cd_ph[mw]_ map)
            { "Kliff_Glasses",                               "cd_phm_00_glasses_0002"            }, // Glasses (Kliff fallback)
            { "Kliff_Mask",                                  "cd_phm_00_mask_00_0271_a"          }, // Mask    (cross-char)
            { "Aggro_Backpack",                              "cd_phm_00_bag_0053"                }, // Backpack (Kliff fallback)
            { "Damian_Daeil_Band",                           "cd_phw_00_rinkband_0001"           }, // Bracelet (phw, female rig)
            { "Demian_OneHandRapier",                        "cd_phw_01_sword_0005"              }, // MainHand
            { "Damian_OneHandShield",                        "cd_phw_03_shield_0131"             }, // OffHand (shield off-hand)
            { "Damian_OneHandPistol",                        "cd_phm_06_pistol_0001"             }, // Ranged
            { "Rikisis_OneHandDagger",                       "cd_phm_01_dagger_0004_r"           }, // SubWeapon
            { "Tynion_Giant_TwoHandGiantBastard",            "cd_phw_02_sword_0002"              }, // TwoHandWeapon
        },

        // ============================================================
        // Oongka (male orc). prefabName values derived from the
        // save-editor companion repo's iteminfo dump. Orc-tribe assets
        // share the cd_phm_* family (orc model is male-tier). Bilibili
        // earring/ring use _index## minor variants; LT's body-mesh
        // hook registers all variants of the base prefab.
        // ============================================================
        {
            { "Lardein_Fabric_Helm",                   "cd_phm_00_hel_0122_01_index01"                }, // Helm
            { "Oongka_Basic_Leather_Armor",                      "cd_phm_00_ub_00_0056"              }, // Chest
            { "Oongka_Basic_Leather_Cloak",                      "cd_phm_00_cloak_00_0056_t"         }, // Cloak
            { "Oongka_Basic_Leather_Gloves",                     "cd_phm_00_hand_00_0056"               }, // Gloves
            { "Langust_Leather_Boots",                           "cd_phm_00_foot_0056"               }, // Boots
            { "Bilibili_Earring",                            "cd_phm_00_earring_0008_l_index02"  }, // Earring1
            { "WhiteHorn_Earring",                           "cd_phm_00_earring_0014_l"          }, // Earring2
            { "Bilibili_Necklace",                           "cd_phm_00_necklace_0004_index02"   }, // Necklace
            { "Pailune_Nobility_Degree_I",                   "cd_phm_00_ring_0006_r_index05"     }, // Ring1
            { "Bilibili_Ring",                               "cd_phm_00_ring_0005_l_index02"     }, // Ring2
            { "Lantern",                                     ""                                  }, // Lantern (cd_t0000_* family, not in cd_ph[mw]_ map)
            { "",                                            ""                                  }, // Glasses (no Oongka capture)
            { "Kliff_Mask",                                  "cd_phm_00_mask_00_0271_a"          }, // Mask    (cross-char)
            { "Oongka_Rocket_BackPack",                      "cd_phm_00_bag_0057"                }, // Backpack (orc-tribe rocket pack)
            { "OOngka_Daeil_Band",                           "cd_pom_00_rinkband_0001"           }, // Bracelet (pom, orc rig)
            { "Big_Horn_Tiger_OneHandAxe",                   "cd_phm_01_axe_0035_r"              }, // MainHand
            { "Big_Horn_Tiger_OneHandAxe",                   "cd_phm_01_axe_0035_r"              }, // OffHand (1H mirror)
            { "Orc_OneHandCannon",                           "cd_phm_13_cannon_0003_l"           }, // Ranged
            { "Aurio_OneHandDagger",                         "cd_phm_01_dagger_0066_r"           }, // SubWeapon
            { "Khadion_TwoHandSword",                        "cd_phm_02_sword_0010"              }, // TwoHandWeapon
        },
    };
    // clang-format on

    // Lookup helpers ----------------------------------------------------

    inline std::optional<CarrierChar>
    carrier_char_from_name(std::string_view name) noexcept
    {
        if (name == "Kliff")
            return CarrierChar::Kliff;
        if (name == "Damiane")
            return CarrierChar::Damiane;
        if (name == "Oongka")
            return CarrierChar::Oongka;
        return std::nullopt;
    }

    inline constexpr const CarrierDefault &
    carrier_for(CarrierChar c, TransmogSlot s) noexcept
    {
        return k_carriers[static_cast<std::size_t>(c)]
                         [static_cast<std::size_t>(s)];
    }
} // namespace Transmog
