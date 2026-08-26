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
    //               variant list (itemmesh_dumper::variant_meshes_for_item) through
    //               PrefabWrapperSwap::carrier_source_seed. A hardcoded prefab column drifts from the itemName each
    //               patch. The runtime variant list is always exact.
    struct CarrierDefault
    {
        const char *itemName;
    };

    // Character axis. The order is fixed, because CarrierChar(i) maps to k_carriers[i]. New characters append at the
    // end, before Count.
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
        // Kliff (male). Armor slots use the Kairos plate set (`Kliff_PlateArmor_*`); the remaining slots come from the
        // live slot-discovery dump.
        //
        // Every armor carrier must RENDER on its own. The prefab-wrapper swap supplies the visual by redirecting the
        // carrier's own mesh, so a carrier whose prefab never resolves to a live wrapper produces an EMPTY slot
        // rather than a swapped one.
        //
        // The Kairos set is chosen for that: every piece is catalog-resident, non-variant, and resolves to a bare
        // prefab name that yields a real wrapper. The previous helm carrier (`Scovi_Fabric_Helm`) is the exact failure
        // this guards against -- the same bare-helm-name no-op already documented for Oongka's Lardein helm below.
        // ============================================================
        {
            { "Kliff_PlateArmor_Helm"                        }, // Helm
            { "Kliff_PlateArmor_Armor"                       }, // Chest
            { "Kliff_PlateArmor_Cloak"                       }, // Cloak
            { "Kliff_PlateArmor_Gloves"                      }, // Gloves
            { "Kliff_Plate_Boots"                            }, // Boots
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
            // Tool / OffHand2 / Ranged2 are deliberately left without a carrier until their slots are enabled.
            //
            // The blocker is carrier COLLISION, not the disabled flag. A name in this table is seeded as that slot's
            // prefab-swap source, and the seeding pass walks every row without consulting `SlotMetadata::enabled`, so
            // ten already-disabled slots carry names here safely. What none of them do is name the same item in two
            // slots that `slots_share_prefab_family` does not pair: every duplicate in this table (the MainHand and
            // OffHand mirrors, Ring1 and Ring2) sits inside a declared pair, so the swap layer knows the two records
            // reach one wrapper. OffHand2 and Ranged2 have no such pairing, and their natural carriers are exactly the
            // items OffHand and Ranged already use, so filling them in publishes one wrapper through two unrelated
            // records. An empty name is skipped by the seeding pass, which keeps the slot inert instead.
            //
            // Carriers observed in live auth-table dumps, recorded here so the values are not lost:
            //   Tool      Kliff `Equip_Felling_Axe`, Damiane `Equip_Shovel`. The tool family is character-agnostic, so
            //             one item can serve all three rows. Collides with nothing.
            //   OffHand2  Kliff `Legendary_MarniTank_OneHandShield`. Unique, would not collide.
            //   Ranged2   Kliff `GreyWolf_OneHandBow`. Collides with his Ranged carrier.
            // Damiane and Oongka have no observed OffHand2 or Ranged2 item; anything put there today is a guess.
            //
            // To enable a slot: fill its cell, and either choose a carrier no other slot in the same character row
            // uses, or add the slot pairing to `slots_share_prefab_family` so the shared wrapper is modelled.
            { ""                                             }, // Tool     (see note above)
            { ""                                             }, // OffHand2 (see note above)
            { ""                                             }, // Ranged2  (see note above)
        },

        // ============================================================
        // Damiane (female). Demeniss Elite/Uniform Leather armor set + Pattern jewelry set + Damian_OneHandPistol
        // Ranged. The engine uses cd_phw_* for female-specific assets and cd_phm_* for shared accessories. PWS derives
        // the source rig meshes from each carrier itemId at runtime.
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
            // Observed: Tool `Equip_Shovel`. No observed OffHand2 or Ranged2 item. See the Kliff row for why all
            // three stay empty.
            { ""                                             }, // Tool
            { ""                                             }, // OffHand2
            { ""                                             }, // Ranged2
        },

        // ============================================================
        // Oongka (male orc). Orc assets share the cd_phm_* family (the orc model is male-tier). PWS derives the source
        // rig meshes from each carrier itemId at runtime. The `_dd` runtime-wrapper suffix note below still applies to
        // how the picker matches source wrappers.
        //
        // NOTE (helm `_dd` suffix): the prefab-swap resolver (heap_walk_partprefab_for_names) matches src by EXACT
        // strcmp against LIVE partprefab wrapper names. The helm slot is special (see prefab_wrapper_swap.cpp
        // k_helmSlotId comment) -- the engine instantiates the default helm variant's runtime wrapper with a `_dd`
        // suffix. So for hel_0122's index01 the ONLY live wrapper is `cd_phm_00_hel_0122_01_index01_dd`. The bare
        // `..._index01` exists only in string/data tables, never as a wrapper, so a bare helm name never resolves and
        // the helm mesh-swap turns into a silent no-op. Other slots use bare names and resolve correctly, because the
        // `_dd` quirk is helm-specific. On patch day, verify the exact live wrapper name against the live partprefab
        // pool: the bare name must yield no wrapper, and the `_dd` name must yield one.
        // ============================================================
        {
            // Oongka's plate set exists only in tiered form (_II Valortread, _III Belkandor); there is no bare
            // `Oongka_PlateArmor_Helm`. Both tiers are complete five-piece sets, so unlike Kliff this row needs no
            // odd boots exception.
            { "Oongka_PlateArmor_Helm_III"                   }, // Helm
            { "Oongka_PlateArmor_Armor_III"                  }, // Chest
            { "Oongka_PlateArmor_Cloak_III"                  }, // Cloak
            { "Oongka_PlateArmor_Gloves_III"                 }, // Gloves
            { "Oongka_PlateArmor_Boots_III"                  }, // Boots
            { "Bilibili_Earring"                             }, // Earring1
            { "WhiteHorn_Earring"                            }, // Earring2
            { "Bilibili_Necklace"                            }, // Necklace
            { "Pailune_Nobility_Degree_I"                    }, // Ring1
            { "Bilibili_Ring"                                }, // Ring2
            { "Lantern"                                      }, // Lantern (cd_t0000_* family, not in cd_ph[mw]_ map)
            { "Kliff_Glasses"                                }, // Glasses (Kliff fallback -- shared accessory)
            { "Kliff_Mask"                                   }, // Mask    (cross-char)
            { "Oongka_Rocket_BackPack"                       }, // Backpack (orc rocket pack)
            { "OOngka_Daeil_Band"                            }, // Bracelet (pom, orc rig)
            { "Big_Horn_Tiger_OneHandAxe"                    }, // MainHand
            { "Big_Horn_Tiger_OneHandAxe"                    }, // OffHand (1H mirror)
            { "Orc_OneHandCannon"                            }, // Ranged
            { "Aurio_OneHandDagger"                          }, // SubWeapon
            { "Khadion_TwoHandSword"                         }, // TwoHandWeapon
            // No observed Tool, OffHand2 or Ranged2 item for Oongka. See the Kliff row for why all three stay empty.
            { ""                                             }, // Tool
            { ""                                             }, // OffHand2
            { ""                                             }, // Ranged2
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

    /// Character name for a CarrierChar. Inverse of @ref carrier_char_from_name; the names are the same keys
    /// PresetManager stores per-character state under.
    inline constexpr std::string_view carrier_char_name(CarrierChar c) noexcept
    {
        switch (c)
        {
        case CarrierChar::Kliff:
            return "Kliff";
        case CarrierChar::Damiane:
            return "Damiane";
        case CarrierChar::Oongka:
            return "Oongka";
        default:
            return {};
        }
    }

    inline constexpr const CarrierDefault &carrier_for(CarrierChar c, TransmogSlot s) noexcept
    {
        return k_carriers[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)];
    }
} // namespace Transmog
