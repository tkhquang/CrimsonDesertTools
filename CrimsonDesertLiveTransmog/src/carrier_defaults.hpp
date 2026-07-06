#pragma once

#include "shared_state.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Transmog
{
    // Per-(character, slot) default carrier ITEM used by LT's transmog apply path AND by the prefab-wrapper-swap
    // picker.
    //
    //   itemName -- resolved through ItemNameTable::id_of() at runtime to a uint16_t carrier itemId. Drives
    //               SlotPopulator(itemId) for the carrier-equip path, and is the SOLE input the prefab-wrapper-swap
    //               source needs: PWS derives the carrier's body-mesh source prefab(s) from this item's runtime
    //               variant list (itemmesh_dumper::variant_meshes_for_item -> desc+0x3E0) through
    //               PrefabWrapperSwap::carrier_source_seed, rather than a hardcoded prefab column that could drift
    //               from the itemName each patch; the runtime variant list is always exact.
    struct CarrierDefault
    {
        const char *itemName;
    };

    // Character axis. Order is fixed; CarrierChar(i) maps to k_carriers[i]. New characters append at the end before
    // Count.
    enum class CarrierChar : std::size_t
    {
        Kliff = 0,
        Damiane = 1,
        Oongka = 2,
        Count
    };

    inline constexpr std::size_t k_carrierCharCount = static_cast<std::size_t>(CarrierChar::Count);

    // 2D table indexed [character][slot]. Adding a slot = one column in each character row. Adding a character = one
    // new row.
    // clang-format off
    inline constexpr CarrierDefault k_carriers[k_carrierCharCount][k_slotCount] = {
        // ============================================================
        // Kliff (male). Default carriers per slot from the live slot-discovery dump.
        // ============================================================
        {
            { "Scovi_Fabric_Helm"                            }, // Helm
            { "Mercenary_Leather_Armor"                      }, // Chest
            { "Mercenary_Leather_Cloak"                      }, // Cloak
            { "Mercenary_Gloves"                             }, // Gloves
            { "Mercenary_Leather_Boots"                      }, // Boots
            { "Hexe_Earring"                                 }, // Earring1
            { "Ancient_People_Earring"                       }, // Earring2
            { "Titan_Necklace"                               }, // Necklace
            { "Antumbra_DarkDeacon_Ring"                     }, // Ring1
            { "AbyssReward_EastWitch_Ring"                   }, // Ring2 (variants: _l/_r)
            { "RedNose_Lantern"                              }, // Lantern
            { "Kliff_Glasses"                                }, // Glasses
            { "Kliff_Mask"                                   }, // Mask
            { "Aggro_Backpack"                               }, // Backpack
            { "Daeil_Band"                                   }, // Bracelet (phm, male rig)
            { "Legendary_Dragon_OneHandSword"                }, // MainHand
            { "Legendary_Dragon_OneHandSword"                }, // OffHand (mirror)
            { "GreyWolf_OneHandBow"                          }, // Ranged
            { "Legendary_Shakatu_OneHandDagger"              }, // SubWeapon
            { "Legendary_Antumbra_TwoHandGiantBastard"       }, // TwoHandWeapon
        },

        // ============================================================
        // Damiane (female). Demeniss Elite/Uniform Leather armor set +
        // Pattern jewelry set + Damian_OneHandPistol Ranged. The engine uses cd_phw_* for female-specific assets
        // and cd_phm_* for shared accessories; PWS derives the source rig meshes from each carrier itemId at runtime.
        // ============================================================
        {
            { "Demian_PlateArmor_Helm_VII"                   }, // Helm
            { "Damian_Demeniss_Elite_Uniform_Leather_Armor"  }, // Chest
            { "Damian_Demeniss_Uniform_Leather_Cloak"        }, // Cloak
            { "Damian_Demeniss_Uniform_Leather_Gloves"       }, // Gloves
            { "Damian_Demeniss_Elite_Uniform_Leather_Boots"  }, // Boots
            { "Pattern_Bronze_Earring"                       }, // Earring1
            { "Pattern_Silver_Earring"                       }, // Earring2
            { "Pattern_Copper_Necklace"                      }, // Necklace
            { "Hernand_Nobility_Degree_I"                    }, // Ring1
            { "Hernand_Nobility_Degree_I"                    }, // Ring2 (same item in both rings)
            { "Lantern"                                      }, // Lantern (cd_t0000_* family, not in cd_ph[mw]_ map)
            { "Kliff_Glasses"                                }, // Glasses (Kliff fallback)
            { "Kliff_Mask"                                   }, // Mask    (cross-char)
            { "Aggro_Backpack"                               }, // Backpack (Kliff fallback)
            { "Damian_Daeil_Band"                            }, // Bracelet (phw, female rig)
            { "Demian_OneHandRapier"                         }, // MainHand
            { "Damian_OneHandShield"                         }, // OffHand (shield off-hand)
            { "Damian_OneHandPistol"                         }, // Ranged
            { "Rikisis_OneHandDagger"                        }, // SubWeapon
            { "Tynion_Giant_TwoHandGiantBastard"             }, // TwoHandWeapon
        },

        // ============================================================
        // Oongka (male orc). Orc-tribe assets share the cd_phm_* family (orc model is male-tier). PWS derives the
        // source rig meshes from each carrier itemId at runtime; the `_dd` runtime-wrapper suffix note below still
        // applies to how the picker matches source wrappers.
        //
        // NOTE (helm `_dd` suffix): the prefab-swap resolver (heap_walk_partprefab_for_names) matches src by EXACT
        // strcmp against LIVE partprefab wrapper names. The helm slot is special (see prefab_wrapper_swap.cpp
        // k_helmSlotId comment) -- the engine instantiates the default helm variant's runtime wrapper with a `_dd`
        // suffix. So for hel_0122's index01 the ONLY live wrapper is `cd_phm_00_hel_0122_01_index01_dd`; the bare
        // `..._index01` exists only in string/data tables, never as a wrapper. Verified live (CE): bare name = 0 valid
        // wrappers, `_dd` name = valid wrapper at the 0x41C1E4* pool (same region as the resolved chest/cloak/etc.).
        // The bare form was authored from the itemprefab data dump and silently never resolved -> helm mesh-swap was a
        // no-op. This only surfaced once the carrier render path (char-class bypass) was fixed on 1.13. Other slots use
        // bare names and resolve fine (the `_dd` quirk is helm-specific). Re-verify the exact live wrapper name if a
        // future patch renames it.
        // ============================================================
        {
            { "Lardein_Fabric_Helm"                          }, // Helm (see _dd note above)
            { "Oongka_Basic_Leather_Armor"                   }, // Chest
            { "Oongka_Basic_Leather_Cloak"                   }, // Cloak
            { "Oongka_Basic_Leather_Gloves"                  }, // Gloves
            { "Langust_Leather_Boots"                        }, // Boots
            { "Bilibili_Earring"                             }, // Earring1
            { "WhiteHorn_Earring"                            }, // Earring2
            { "Bilibili_Necklace"                            }, // Necklace
            { "Pailune_Nobility_Degree_I"                    }, // Ring1
            { "Bilibili_Ring"                                }, // Ring2
            { "Lantern"                                      }, // Lantern (cd_t0000_* family, not in cd_ph[mw]_ map)
            { "Kliff_Glasses"                                }, // Glasses (Kliff fallback -- shared accessory)
            { "Kliff_Mask"                                   }, // Mask    (cross-char)
            { "Oongka_Rocket_BackPack"                       }, // Backpack (orc-tribe rocket pack)
            { "OOngka_Daeil_Band"                            }, // Bracelet (pom, orc rig)
            { "Big_Horn_Tiger_OneHandAxe"                    }, // MainHand
            { "Big_Horn_Tiger_OneHandAxe"                    }, // OffHand (1H mirror)
            { "Orc_OneHandCannon"                            }, // Ranged
            { "Aurio_OneHandDagger"                          }, // SubWeapon
            { "Khadion_TwoHandSword"                         }, // TwoHandWeapon
        },
    };
    // clang-format on

    // Lookup helpers ----------------------------------------------------

    inline std::optional<CarrierChar> carrier_char_from_name(std::string_view name) noexcept
    {
        if (name == "Kliff")
            return CarrierChar::Kliff;
        if (name == "Damiane")
            return CarrierChar::Damiane;
        if (name == "Oongka")
            return CarrierChar::Oongka;
        return std::nullopt;
    }

    inline constexpr const CarrierDefault &carrier_for(CarrierChar c, TransmogSlot s) noexcept
    {
        return k_carriers[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)];
    }
} // namespace Transmog
