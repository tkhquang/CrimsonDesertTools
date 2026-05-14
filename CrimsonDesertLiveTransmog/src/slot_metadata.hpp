#pragma once

#include "shared_state.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace Transmog
{
    // Single source of truth for per-slot static metadata. Indexed by
    // TransmogSlot enum value (`SlotMetadata.slot == TransmogSlot(i)`).
    //
    // Adding a new slot = one row added below. Adding a new per-slot
    // attribute = one new field on this struct + one column populated
    // per row. All consumers (transmog_map.cpp, transmog_apply.cpp,
    // prefab_wrapper_swap.cpp, part_show_suppress.cpp) read this
    // table; no parallel slot tables anywhere else.
    struct SlotMetadata
    {
        TransmogSlot   slot;             // == array index, asserted at compile time
        std::int16_t   gameTag;          // engine slot tag (sub_148EB6700-verified)
        const char    *displayName;      // "Helm", "Chest", ... (UI / log labels)
        const char    *prefabPrefixMale; // PWS catalog prefix for male characters
        const char    *prefabPrefixFemale; // PWS catalog prefix for female characters
        // PartShowSuppress IndexedStringA hash key (CD_*). nullptr when
        // the slot does NOT participate in PartShowSuppress (only the
        // 5 armor slots have CD_* hashes; accessory/weapon slots use
        // different suppression mechanisms or none at all).
        const char    *partShowHashKey;
        // Master enable flag. When false, the slot is omitted from the
        // overlay slot picker AND skipped in the apply/clear dispatcher,
        // so even a preset that loaded with `active=true` for this slot
        // becomes a no-op until the flag is flipped back on.
        //
        // Currently disabled slots fall into two unsolved categories:
        //
        // (A) MULTI-PREFAB NON-ARMOR slots (rings, earrings, brackets,
        //     all weapon slots, sub-weapons): a single equipped item
        //     emits MORE THAN ONE prefab into the engine's render list,
        //     so the current "swap one source wrapper -> one target
        //     wrapper" pipeline in prefab_wrapper_swap drops 2nd/3rd
        //     prefabs and leaves the visual half-applied. Needs
        //     refactor to either (1) widen the swap map to N->M or
        //     (2) drive the swap from the auth-table mod array which
        //     already enumerates every contributing prefab.
        //
        // (B) DUPLICATE-TAG slots (Ring1/Ring2 share tag 0x0A/0x0B,
        //     Earring1/Earring2 share 0x07/0x08, MainHand/OffHand
        //     share dual-wield instances of the same item-id): the
        //     SlotPopulator hook upstream sees one record per engine
        //     tag and cannot disambiguate which UI slot the user
        //     targeted. Need to identify the disambiguator field on
        //     the swap-entry record (likely a sub-index byte at a
        //     yet-unmapped offset; engine slot taxonomy memory file
        //     2026-05-07 has the candidate offsets).
        //
        // Both classes are flipped back on by replacing `false` with
        // `true` in the row below AND landing the corresponding
        // refactor; the slot is otherwise fully wired through the
        // metadata table -- no other code paths gate on the slot
        // identity once `enabled` is true.
        bool           enabled;
    };

    inline constexpr SlotMetadata k_slotMetadata[k_slotCount] = {
        // slot               gameTag  displayName       prefabPrefixMale          prefabPrefixFemale       partShowHashKey   enabled
        // -- 5 armor slots: original transmog targets, fully working via PartShowSuppress + cd_phm_/cd_phw_ wrapper swap.
        { TransmogSlot::Helm,          0x03, "Helm",          "cd_phm_00_hel_00_",      "cd_phw_00_hel_00_",     "CD_Helm"      , true  },
        { TransmogSlot::Chest,         0x04, "Chest",         "cd_phm_00_ub_00_",       "cd_phw_00_ub_00_",      "CD_Upperbody" , true  },
        { TransmogSlot::Cloak,         0x10, "Cloak",         "cd_phm_00_cloak_00_",    "cd_phw_00_cloak_00_",   "CD_Cloak"     , true  },
        { TransmogSlot::Gloves,        0x05, "Gloves",        "cd_phm_00_hand_00_",     "cd_phw_00_hand_00_",    "CD_Hand"      , true  },
        { TransmogSlot::Boots,         0x06, "Boots",         "cd_phm_00_foot_00_",     "cd_phw_00_foot_00_",    "CD_Foot"      , true  },
        // -- DISABLED (multi-prefab + duplicate-tag): rings/earrings emit
        //    multiple prefabs per item AND share tags within a pair, so
        //    SlotPopulator can't tell Ring1 from Ring2. See SlotMetadata
        //    `enabled` doc-block above for the refactor plan.
        { TransmogSlot::Earring1,      0x07, "Earring1",      "",                       "",                      nullptr        , false },
        { TransmogSlot::Earring2,      0x08, "Earring2",      "",                       "",                      nullptr        , false },
        // -- Necklace: single-prefab single-tag, treated like an armor slot.
        { TransmogSlot::Necklace,      0x09, "Necklace",      "",                       "",                      nullptr        , true  },
        { TransmogSlot::Ring1,         0x0A, "Ring1",         "cd_phm_00_ring_",        "cd_phw_00_ring_",       nullptr        , false },
        { TransmogSlot::Ring2,         0x0B, "Ring2",         "cd_phm_00_ring_",        "cd_phw_00_ring_",       nullptr        , false },
        // Lantern uses cd_t0000_* on both genders (gender-shared family).
        { TransmogSlot::Lantern,       0x0F, "Lantern",       "cd_t0000_lantern_",      "cd_t0000_lantern_",     nullptr        , true  },
        { TransmogSlot::Glasses,       0x11, "Glasses",       "cd_phm_00_glasses_",     "cd_phw_00_glasses_",    nullptr        , true  },
        { TransmogSlot::Mask,          0x12, "Mask",          "cd_phm_00_mask_00_",     "cd_phw_00_mask_00_",    nullptr        , true  },
        { TransmogSlot::Backpack,      0x13, "Backpack",      "cd_phm_00_bag_0",        "cd_phw_00_bag_0",       nullptr        , true  },
        // Bracelet uses cd_phw_*_rinkband_ on BOTH genders (phw natively,
        // even for male characters -- engine quirk verified live). Marked
        // disabled (`enabled=false`) because it is a multi-prefab,
        // non-armor slot the current pipeline does not handle.
        { TransmogSlot::Bracelet,      0x14, "Bracelet",      "cd_phw_00_rinkband_",    "cd_phw_00_rinkband_",   nullptr        , false },
        // Weapon family slots below are marked disabled (`enabled=false`):
        // every weapon slot emits multiple prefabs per item (mesh +
        // scabbard + FX + binds), and MainHand/OffHand share tag space
        // when dual-wielding. A separate cd_phw_* weapon pipeline plus
        // sub-index disambiguation in SlotPopulator is required before
        // re-enabling them.
        { TransmogSlot::MainHand,     0x00, "MainHand",     "cd_phm_01_",             "cd_phw_01_",            nullptr        , false },
        { TransmogSlot::OffHand,      0x01, "OffHand",      "cd_phm_01_",             "cd_phw_01_",            nullptr        , false },
        { TransmogSlot::Ranged,        0x02, "Ranged",        "cd_phm_04_",             "cd_phw_04_",            nullptr        , false },
        { TransmogSlot::SubWeapon,     0x0C, "SubWeapon",     "cd_phm_01_dagger_",      "cd_phw_01_dagger_",     nullptr        , false },
        // displayName intentionally trimmed to "TwoHand" (was "TwoHandWeapon")
        // to fit the overlay's slot column. Used everywhere `slot_name()`
        // is read; engine-side diagnostics still call this slot's gameTag
        // 0x0D which game_slot_name returns this same trimmed string for.
        { TransmogSlot::TwoHandWeapon, 0x0D, "TwoHand",       "cd_phm_02_",             "cd_phw_02_",            nullptr        , false },
        // Tag 0x0E "Experimental" and 0x15 "OongkaRocket" intentionally
        // omitted -- see TransmogSlot enum comments in shared_state.hpp.
    };

    static_assert(sizeof(k_slotMetadata) / sizeof(k_slotMetadata[0]) == k_slotCount,
                  "k_slotMetadata length must match TransmogSlot::Count");

    // Compile-time index/slot drift check. The constexpr loop expands
    // into per-row static_asserts so a misordered or duplicated row is
    // caught at build time rather than at runtime.
    namespace detail
    {
        constexpr bool slot_metadata_indices_match()
        {
            for (std::size_t i = 0; i < k_slotCount; ++i) {
                if (k_slotMetadata[i].slot != static_cast<TransmogSlot>(i))
                    return false;
            }
            return true;
        }
    }
    static_assert(detail::slot_metadata_indices_match(),
                  "k_slotMetadata row order must match TransmogSlot enum order "
                  "(SlotMetadata.slot at row i must equal TransmogSlot(i)).");

    // Direct accessor by enum value. O(1).
    inline constexpr const SlotMetadata &slot_meta(TransmogSlot s) noexcept
    {
        return k_slotMetadata[static_cast<std::size_t>(s)];
    }

    // Master enable check. Disabled slots are hidden from the slot
    // picker and short-circuited in the apply/clear dispatcher. See
    // SlotMetadata::enabled doc-block for the refactor blockers.
    inline constexpr bool slot_enabled(TransmogSlot s) noexcept
    {
        return slot_meta(s).enabled;
    }
    inline constexpr bool slot_enabled(std::size_t i) noexcept
    {
        return i < k_slotCount && k_slotMetadata[i].enabled;
    }

    // Reverse lookup: engine slot tag -> TransmogSlot. Returns
    // std::nullopt for tags LT does not manage (e.g. 0x0E or 0x15).
    // Linear search over 20 entries -- cheap, called sparingly.
    inline std::optional<TransmogSlot> slot_from_game_tag(std::int16_t gameTag) noexcept
    {
        for (const auto &m : k_slotMetadata) {
            if (m.gameTag == gameTag)
                return m.slot;
        }
        return std::nullopt;
    }

    // Map a body-mesh prefab name to its logical slot. Substring-based
    // so it works for ALL role-prefix variants:
    //
    //   Player:    cd_phm_*, cd_phw_*       (human male/female)
    //   NPC:       cd_nhm_*, cd_nhw_*, cd_ndm_*
    //   Other:     cd_pdm_*, cd_pgm_*, cd_pom_*, cd_ptm_*
    //   Accessory: cd_t0000_lantern_*       (no role prefix)
    //   Mount:     cd_m0001_*               (suffix-driven)
    //
    // The numeric role chunk `_NN_` (e.g. `_02_` for 2H weapons,
    // `_13_` for 1H cannons) is extracted regardless of the role
    // prefix so a future `cd_xxx_02_sword_*` family inherits the 2H
    // classification automatically. Specific weapon-type tags
    // (dagger, alebard, pike, ...) are checked first because they
    // have unambiguous slot mappings; ambiguous tags (sword, axe,
    // hammer, cannon, mace, lance, fist) fall through to the numeric
    // role for 1H vs 2H vs ranged disambiguation.
    //
    // Returns std::nullopt for prefabs that match nothing the
    // picker should label (e.g. environmental meshes, food items).
    inline std::optional<TransmogSlot>
    slot_for_prefab_name(const std::string &name) noexcept
    {
        const auto has = [&](const char *tag) noexcept {
            return name.find(tag) != std::string::npos;
        };

        // Reject UI/knowledge/icon assets that embed a real prefab as a
        // substring (e.g.
        // "cd_knowledgeimage_Knowledge_ItemIcon_Prefab_cd_phm_00_hel_00_0363_c"
        // would otherwise match `_hel_` and pollute the Helm Exact
        // list). Body-mesh prefab names are all-lowercase snake_case by
        // engine convention -- any uppercase ASCII letter means the
        // name is a UI / knowledge / icon asset that happens to embed
        // an armor prefab in its identifier. Structural rather than
        // allowlist-based so new lowercase role families (mounts,
        // future NPC variants, etc.) keep working without a code change.
        for (unsigned char c : name) {
            if (c >= 'A' && c <= 'Z')
                return std::nullopt;
        }
        // Real prefab names also start with the engine's `cd_` family
        // marker. Reject anything that doesn't (cheap sanity check
        // against accidentally-lowercased UI strings).
        if (name.size() < 3 ||
            name[0] != 'c' || name[1] != 'd' || name[2] != '_')
            return std::nullopt;

        // Extract the first `_NN_` (two-digit) numeric role marker
        // after the `cd_<role>_` prefix. -1 when absent (NPC, t0000,
        // m0001 families). Cheap manual scan; no <regex> dependency.
        int roleNum = -1;
        for (std::size_t i = 0; i + 4 <= name.size(); ++i) {
            if (name[i] == '_' &&
                std::isdigit(static_cast<unsigned char>(name[i + 1])) &&
                std::isdigit(static_cast<unsigned char>(name[i + 2])) &&
                name[i + 3] == '_') {
                roleNum = (name[i + 1] - '0') * 10 +
                          (name[i + 2] - '0');
                break;
            }
        }

        // --- Specific-name weapons (unambiguous slot) ---
        if (has("_dagger_"))
            return TransmogSlot::SubWeapon;
        if (has("_alebard_") || has("_pike_") ||
            has("_spear_")   || has("_greekfire_") ||
            has("_icethrower_") || has("_lightningthrower_") ||
            has("_warhammer_") || has("_flexiblewarhammer_"))
            return TransmogSlot::TwoHandWeapon;
        if (has("_kiteshield_") || has("_towershield_") ||
            has("_shield_"))
            return TransmogSlot::OffHand;
        if (has("_arw_")     || has("_bow_") ||
            has("_crossbow_") || has("_pistol_") ||
            has("_musket_")  || has("_shotgun_") ||
            has("_bomb_")    || has("_blowpipe_"))
            return TransmogSlot::Ranged;

        // --- Ambiguous weapon tags: disambiguate by numeric role ---
        // Convention from the v1.05.01 prefab catalog (TSV scan):
        //   _01_ = 1H, _02_ = 2H, _03_ = shield, _04..06,08,10,13 = ranged.
        if (has("_sword_") || has("_axe_") || has("_mace_") ||
            has("_lance_") || has("_hammer_") || has("_cannon_") ||
            has("_fist_")) {
            switch (roleNum) {
                case 2: case 12:
                    return TransmogSlot::TwoHandWeapon;
                case 4: case 5: case 6: case 8: case 10: case 13:
                    return TransmogSlot::Ranged;
                default:
                    return TransmogSlot::MainHand;
            }
        }

        // --- Body parts (substring tags work across all role prefixes) ---
        if (has("_hel_"))      return TransmogSlot::Helm;
        if (has("_ub_"))       return TransmogSlot::Chest;
        if (has("_cloak_"))    return TransmogSlot::Cloak;
        if (has("_hand_"))     return TransmogSlot::Gloves;
        if (has("_foot_"))     return TransmogSlot::Boots;
        if (has("_earring_"))  return TransmogSlot::Earring1;
        if (has("_necklace_")) return TransmogSlot::Necklace;
        if (has("_rinkband_")) return TransmogSlot::Bracelet;
        if (has("_ring_"))     return TransmogSlot::Ring1;
        if (has("_lantern_"))  return TransmogSlot::Lantern;
        if (has("_glasses_"))  return TransmogSlot::Glasses;
        if (has("_mask_"))     return TransmogSlot::Mask;
        if (has("_bag_"))      return TransmogSlot::Backpack;

        return std::nullopt;
    }

    // Picker-side compatibility: when the user opens slot X's picker
    // in prefab mode with the Exact filter on, prefabs whose
    // `slot_for_prefab_name()` is in the same equivalence group as X
    // should still pass. Pair-slots (Ring1/Ring2, Earring1/Earring2,
    // MainHand/OffHand) share their body-mesh family.
    inline bool slots_share_prefab_family(TransmogSlot a,
                                          TransmogSlot b) noexcept
    {
        if (a == b) return true;
        const auto pair = [](TransmogSlot x, TransmogSlot p1,
                             TransmogSlot p2) noexcept {
            return x == p1 || x == p2;
        };
        if (pair(a, TransmogSlot::Earring1, TransmogSlot::Earring2) &&
            pair(b, TransmogSlot::Earring1, TransmogSlot::Earring2))
            return true;
        if (pair(a, TransmogSlot::Ring1, TransmogSlot::Ring2) &&
            pair(b, TransmogSlot::Ring1, TransmogSlot::Ring2))
            return true;
        if (pair(a, TransmogSlot::MainHand, TransmogSlot::OffHand) &&
            pair(b, TransmogSlot::MainHand, TransmogSlot::OffHand))
            return true;
        return false;
    }
} // namespace Transmog
