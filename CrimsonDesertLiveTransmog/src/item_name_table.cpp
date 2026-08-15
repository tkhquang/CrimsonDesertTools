#include "item_name_table.hpp"
#include "aob_resolver.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <string_view>
#include <mutex>
#include <unordered_map>

namespace Transmog
{
    // Sensible upper bound on item descriptor catalog size. The game ships several thousand entries. Guard generously.
    static constexpr uint32_t k_maxCatalogSize = 0x20000;

    static constexpr std::size_t k_maxNameLen = 96;

    // Variant-metadata detection (see item_name_table.hpp::has_variant_meta). Clean base items have `*(desc+<offset>)
    // == <sentinel>`, where <sentinel> is a shared empty-object pointer -- an IRefCounted vtable in the exe's .data
    // section. Non-sentinel values point to a per-item metadata struct threaded through a catalog-wide linked list.
    // Members of that list do not render via runtime transmog.
    //
    // The field offset moves whenever the descriptor struct changes shape. The region grows on some patches and
    // SHRINKS on others, so do not extrapolate a direction from the last move. A stale offset fails SILENTLY: the
    // neighboring qwords hold plausible values too (a constant 0, or an unrelated bit pattern such as 0x1FFFFFFFF), so
    // nothing trips and every item classifies wrong.
    //
    // Re-derive the offset like this. Probe every descriptor qword across a 0x380..0x480 window, then correlate
    // "value != mode" for each candidate offset against the Variant column of the previous game version's item dump,
    // matched BY NAME. The correct offset produces no false positives. No other offset in the window comes close, so
    // the result is unambiguous. Note that at least one other descriptor offset also holds a sentinel on every item,
    // so "most items share this value" alone does NOT identify the field. The distinguishing constraint is: sentinel
    // for direct-wear items, per-item heap pointer for carrier-required items.
    //
    // The sentinel value itself is also unstable, because any .data reshuffle moves it. Rather than hardcoding either
    // the offset or the sentinel address, the builder resolves the sentinel statistically at catalog-build time. It
    // scans every valid descriptor's qword at this offset, and the value that appears in the clear majority of items
    // IS the sentinel. This self-heals across future game updates as long as the catalog stays statistically dominated
    // by base items.
    static constexpr std::ptrdiff_t k_descVariantMetaOffset = 0x3B0;

    // Canonical item-type code, u16. This is the field the engine itself reads to classify an item: the SlotPopulator
    // loads the descriptor through the item accessor and then runs `movzx ecx, word ptr [desc+<this offset>]` to index
    // its EquipTypeInfo table. Slot derivation follows the same field so the mod and the engine agree.
    //
    // The offset moves when the descriptor head changes width. A wrong value fails SILENTLY, because a neighboring
    // offset still reads as a plausible u16. Verify it the same way it was derived: walk every descriptor, build a
    // value histogram, and keep the offset whose values cluster by item family. The correct field yields on the order
    // of a hundred distinct codes with large per-family runs (helms, chests, gloves, boots) plus one dominant 0xFFFF
    // bucket for arrows and quest items. A wrong offset yields either a near-constant value or hundreds of singleton
    // values that are byte fragments of an adjacent pointer.
    static constexpr std::ptrdiff_t k_descTypeCodeOffset = 0x42;

    // --- iteminfo container layout ---
    // These are runtime data offsets, not code, so they cannot be AOB-scanned. If a future patch reshapes the struct,
    // the catalog walk produces an implausible count or ptrArray and bails at the sanity checks below. Read the live
    // values off the `mov rax,[rbx+<offset>]` that ItemAccessor uses to reach the array.
    //
    // WARNING -- the sanity checks do NOT catch a small shift of the array offset. The offset moves when the
    // pa::StaticInfoManager2 base that iteminfo derives from changes width. Both the old and the new holder offsets
    // dereference to valid-looking heap pointers, so the `ptrArray < 0x10000` guard stays silent and the walk emits
    // garbage item names instead of failing. The StringInfo registry rides the same base and moves with it (see
    // prefab_wrapper_swap.cpp), but a change to that base does NOT move every member by the same amount, and it does
    // not always move them in the same direction. The count offset and the array offset move independently. Verify
    // each one against live memory on patch day rather than trusting the guards.
    static constexpr std::ptrdiff_t k_iteminfoCountOffset = 0x08;    // dword entry count
    static constexpr std::ptrdiff_t k_iteminfoPtrArrayOffset = 0x58; // qword base of descriptor ptr array

    // Resolved sentinel, cached after the first successful build(). 0 means "not yet resolved". Until the next
    // build() populates it, has_variant_meta() falls back to false. That lets a bad item through instead of
    // mis-flagging a clean one.
    static std::atomic<uintptr_t> s_variantMetaSentinel{0};

    // --- Safe memory helpers ---
    //
    // The `(value, bool& ok)` shape distinguishes a faulted read from a legitimate zero result, which matters at call
    // sites where 0 is a valid value (e.g. slot index 0 versus unread slot field). `DMKMemory::seh_read<T>` is the
    // underlying SEH-protected primitive. These adapters fold its `std::optional<T>` return into the local shape used
    // by the rest of this translation unit.

    static uint8_t read_u8_safe(uintptr_t addr, bool &ok) noexcept
    {
        const auto v = DMKMemory::seh_read<uint8_t>(addr);
        ok = v.has_value();
        return v.value_or(0);
    }

    static int32_t read_i32_safe(uintptr_t addr, bool &ok) noexcept
    {
        const auto v = DMKMemory::seh_read<int32_t>(addr);
        ok = v.has_value();
        return v.value_or(0);
    }

    static uintptr_t read_qword_safe(uintptr_t addr, bool &ok) noexcept
    {
        const auto v = DMKMemory::seh_read<uintptr_t>(addr);
        ok = v.has_value();
        return v.value_or(0);
    }

    static uint32_t read_u32_safe(uintptr_t addr, bool &ok) noexcept
    {
        const auto v = DMKMemory::seh_read<uint32_t>(addr);
        ok = v.has_value();
        return v.value_or(0);
    }

    static uint16_t read_u16_safe(uintptr_t addr, bool &ok) noexcept
    {
        const auto v = DMKMemory::seh_read<uint16_t>(addr);
        ok = v.has_value();
        return v.value_or(0);
    }

    /**
     * Decode a relative-call instruction ( `E8 disp32` ) at the given address and return its target. Returns 0 on
     * failure.
     */
    static uintptr_t decode_rel_call(uintptr_t callSite) noexcept
    {
        bool ok = false;
        auto opcode = read_u8_safe(callSite, ok);
        if (!ok || opcode != 0xE8)
            return 0;
        auto disp = read_i32_safe(callSite + 1, ok);
        if (!ok)
            return 0;
        return callSite + 5 + static_cast<intptr_t>(disp);
    }

    /**
     * Scan the first `scanBytes` of a function for the first `E8 disp32` call and return its target. Returns 0 on
     * failure.
     */
    static uintptr_t first_rel_call_target(uintptr_t funcStart, std::size_t scanBytes) noexcept
    {
        for (std::size_t off = 0; off + 5 <= scanBytes; ++off)
        {
            bool ok = false;
            auto opcode = read_u8_safe(funcStart + off, ok);
            if (!ok)
                return 0;
            if (opcode == 0xE8)
                return decode_rel_call(funcStart + off);
        }
        return 0;
    }

    /**
     * Read a null-terminated ASCII string from `strPtr` into `buf`.
     */
    static std::size_t read_cstring_safe(uintptr_t strPtr, char *buf, std::size_t bufSize) noexcept
    {
        __try
        {
            const auto *src = reinterpret_cast<const char *>(strPtr);
            std::size_t len = 0;
            while (len < bufSize - 1 && src[len] != '\0')
            {
                const auto c = src[len];
                // Reject control bytes (0x01..0x1F) -- they signal a misaligned heap read. Accept 0x80..0xFF: some
                // legitimate string_keys are UTF-8 encoded (e.g. Roman numerals in Goblin_Merchant_Fabric_Armor_* use
                // the sequence `E2 85 A2..A5`), and a printable-ASCII-only filter silently drops them.
                if (static_cast<unsigned char>(c) < 0x20)
                    return 0;
                buf[len] = c;
                ++len;
            }
            buf[len] = '\0';
            return len;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // --- Slot classification ---
    //
    // Driven entirely by the canonical item-type code at desc+k_descTypeCodeOffset as captured during the catalog
    // build. Name-parsing heuristics are NOT used: they produce false positives on non-armor items whose names contain
    // tokens like "_Armor_" (horse armors, shields, quest treasure maps, etc). The game's own type code is
    // unambiguous. The fallback is to treat anything unmapped as non-equipment.

    // Slot mapping for the canonical item-type code at desc+k_descTypeCodeOffset.
    //
    // Armor block (the most stable codes -- they sit at the bottom of the engine enum and stay put across patches):
    //   0x04=Helm  0x05=Chest  0x06=Gloves  0x07=Boots
    //
    // Accessories. Earring/Necklace/Ring/Lantern also sit below the shifting band and stay put. Backpack..Mask,
    // Cloak included, ride the band described at the `case` labels below and are NOT stable across patches:
    //   0x08=Earring (Earring1/Earring2 share)
    //   0x09=Necklace
    //   0x0A=Ring (Ring1/Ring2 share)
    //   0x38=Lantern
    //   0x48=Backpack
    //   0x4B=Cloak
    //   0x4C=Bracelet  0x4D=Glasses  0x4E=Mask
    //
    // Weapons (typeCode varies by weapon FAMILY and sometimes by character variant -- all sharing the
    // MainHand/OffHand/Ranged/ SubWeapon/TwoHandWeapon auth-table slots):
    //   Weapon families come from iterating every descriptor's typeCode
    //   against sample item names. Live-equipped items give ground truth
    //   for confirmed entries. The rest come from sample-name conventions
    //   (e.g. *_TwoHandHammer = TwoHandWeapon, *_OneHandShotgun = Ranged).
    //   Items whose slot cannot be inferred from the name stay unmapped.
    //   Runtime observation handles them when the user equips one.
    //
    //   1H families (MainHand, paired-pickered with OffHand):
    //     0x00=1H Sword            0x01=OneHandShield (generic)
    //     0x02=Damiane Shield      0x10=1H Axe
    //     0x13=1H Mace             0x1D=Fist (Item_Fist_*)
    //     0x20=Tower Shield        0x28=Rapier
    //     0x31=Knuckle/Ring_Drill
    //   Ranged:
    //     0x03=Bow                 0x0D=Sprayer (named BackPack, filed ranged by the engine)
    //     0x22=Pistol              0x23=Musket
    //     0x24=Shotgun             0x26=1H Cannon
    //     0x30=Crossbow            0x32=FlameThrower
    //     0x33=IceThrower          0x34=LightningThrower
    //     0x6A=FishingRod
    //   Sub-weapons:
    //     0x0E=Dagger
    //   2H weapons:
    //     0x0F=2H Axe              0x11=Greatsword A
    //     0x12=Greatsword B (NPC)  0x14=WarHammer
    //     0x15=2H Spear/Polearm    0x1C=2H Hammer
    //     0x1F=Chainsaw            0x21=Halberd/Alebard
    //     0x27=2H Cannon           0x35=BlowPipe
    //   Tools (their own engine slot, not the two-hand slot, even though the meshes are held in both hands):
    //     0x1E=Drill               0x5A=Pickaxe
    //     0x5B=Iron Chain          0x5C=Rake
    //     0x5D=Felling Axe (Boss_Reward_SuperWeapon)
    //     0x5E=Shovel              0x5F=Broom
    //     0x61=Hoe                 0x62=Sickle/Scythe
    //     0x63=Work Hammer         0x65=Saw
    //     0x67=Drum                0x68=Stick
    //     0x6C=PriestWand          0x6E=Crutch
    //
    //   Intentionally NOT mapped (not character transmog):
    //     0x0B Oongka_Rocket_Helm (TransmogSlot::OongkaRocket commented)
    //     0x17 Contribution_Flag, 0x18 Torch, 0x2C Witch_WingFan
    //     0x3B-0x3D PetArmor (pet/cat equipment)
    //     0x3E-0x42 HorseArmor (mount equipment), 0x43 animal body armor
    //     0x4F-0x58 WarRobot body parts (mech, not character), 0x59 dragon armor
    //     Notepad/Pen, FlatBasket, Bucket, Pot_Head (household props). Their
    //       codes sit inside the shifting tool band described below, so read
    //       them off the [catalog-histogram] samples instead of trusting a
    //       written value.
    //     0xFFFF Arrow/Quiver/Quest items
    //
    // The 0x11 vs 0x12 split for 2H bastard swords looks like a character/body variant axis. They are the "same"
    // weapon class visually, but the engine tags them differently per protagonist. Both go in the TwoHandWeapon slot.
    //
    // More weapon-family typeCodes will surface as users equip new classes (crossbow, polearm, etc). Runtime
    // `record_observed_slot` covers any unmapped family automatically once an item appears in the auth-table.
    //
    // Excluded by design (not LT-targeted):
    //   0x0B=OongkaRocket Helm. The OongkaRocket TransmogSlot is not
    //        mapped, so leaving 0x0B unmapped keeps those items out of
    //        every picker.
    //
    // For paired slots (weapons/earrings/rings) the static map points at the lower-indexed half of the pair. The picker
    // UI uses `slots_share_picker` so both halves of a pair show the same items. The actual auth-table slot used at
    // apply time is whichever TransmogSlot row the user committed against.
    //
    // Other observed codes rejected: pet and mount armor, WarRobot parts, dragon armor, and 0xFFFF quest /
    // non-equipment.
    static TransmogSlot slot_from_type_code(std::uint16_t code) noexcept
    {
        switch (code)
        {
        // Armor
        case 0x04:
            return TransmogSlot::Helm;
        case 0x05:
            return TransmogSlot::Chest;
        case 0x06:
            return TransmogSlot::Gloves;
        case 0x07:
            return TransmogSlot::Boots;
        // The accessory band (Backpack..Mask) shifts as a BLOCK whenever the engine's item-type enum gains or loses an
        // entry below it. The shift went in both directions across past patches, so do not assume a direction and do
        // not extrapolate one. Armor (Helm..Boots 0x04..0x07) and Earring/Necklace/Ring/Lantern sit below the band and
        // stay put.
        //
        // Re-derive the band by diffing a fresh catalog dump against the previous one BY NAME, or by reading the
        // sample names in the [catalog-histogram] log lines. A stale band is not merely a miss. The codes the band
        // vacates belong to WarRobot parts, so a stale table lists mech parts in the Glasses and Mask pickers while
        // every real bracelet, pair of glasses and mask falls through to unmapped.
        //
        // Superseded values are dropped rather than kept for back-compat, because a one-code shift makes the old
        // values collide with the current ones (an old Cloak code becomes the current Backpack code, and so on). The
        // auto-updating live game only ever presents the current build's codes.
        case 0x4B:
            return TransmogSlot::Cloak; // Soldier_General_Fabric_Cloak, WellsKnight_PlateArmor_Cloak
        case 0x08:
            return TransmogSlot::Earring1; // shared with Earring2
        case 0x09:
            return TransmogSlot::Necklace;
        case 0x0A:
            return TransmogSlot::Ring1; // shared with Ring2
        case 0x38:
            return TransmogSlot::Lantern;
        case 0x48:
            return TransmogSlot::Backpack; // Aggro_Backpack, Bleed_Bomb_BackPack, WaterPower_BackPack
        case 0x4C:
            return TransmogSlot::Bracelet; // Daeil_Band + its OOngka_/Damian_ rig variants
        case 0x4D:
            return TransmogSlot::Glasses; // Kliff_Glasses, Hernand_Crown, Demeniss_Crown
        case 0x4E:
            return TransmogSlot::Mask; // Kliff_Mask
        // Weapon codes below carry the engine's own EquipTypeInfo row name in the trailing comment. That table is the
        // authority: the code IS the row index the engine reads out of the descriptor to classify the item. To re-derive
        // or extend this block, dump the EquipTypeInfo manager's entry array and read each row's name, rather than
        // guessing from item names. A one-hand and two-hand pair often shares a weapon family, so the name is the only
        // reliable way to tell which hand a code belongs to.
        //
        // Codes for non-player families stay UNMAPPED on purpose and fall through to Count: Ammo, HiddenEquip, Cushion,
        // Pet*, Horse*, Robot*, DragonArmor, SpecialVehicleArmor, GhostWeapon, Battery. The mesh binder crashes on
        // non-humanoid rigs, and Count is what keeps them out of the picker.
        //
        // 1H weapons -- MainHand
        case 0x00: // OneHandSword
        case 0x10: // OneHandAxe
        case 0x13: // OneHandMace
        case 0x18: // OneHandTorch
        case 0x1B: // OneHandFlail
        case 0x1D: // OneHandFist
        case 0x1F: // OneHandSaw
        case 0x28: // OneHandRapier
        case 0x2C: // OneHandFan
        case 0x2D: // OneHandHammer
        case 0x31: // Gauntlet
        case 0x36: // OneHandBomb
        case 0x39: // OneHandBola
            return TransmogSlot::MainHand;
        // Shields -- OffHand. The engine files every shield in its own rows, separate from the one-hand weapons above.
        case 0x01: // OneHandShield
        case 0x02: // OneHandShieldRight
        case 0x20: // OneHandTowerShield
            return TransmogSlot::OffHand;
        // Ranged. The spray rig reads as a worn bag from its item name, but the engine files it in the ranged slot and
        // the auth table is the authority, so it classifies as Ranged and not Backpack.
        case 0x03: // OneHandBow
        case 0x0D: // SprayBag
        case 0x22: // OneHandPistol
        case 0x23: // OneHandMusket
        case 0x24: // OneHandShotgun
        case 0x26: // OneHandCannon
        case 0x30: // OneHandCrossBow
        case 0x6A: // ToolFishingRod. Filed here rather than with the tools because it aims and casts.
            return TransmogSlot::Ranged;
        // Sub-weapons
        case 0x0E:
            return TransmogSlot::SubWeapon; // OneHandDagger
        // 2H weapons (combat + utility tools)
        case 0x0F: // TwoHandAxe
        case 0x11: // TwoHandSword
        case 0x12: // TwoHandGiantSword
        case 0x14: // TwoHandWarHammer
        case 0x15: // TwoHandSpear
        case 0x16: // TwoHandGiantSpear
        case 0x17: // TwoHandPike
        case 0x19: // TwoHandRod
        case 0x1A: // TwoHandScythe
        case 0x1C: // TwoHandHammer
        case 0x21: // TwoHandHalberd
        case 0x27: // TwoHandCannon
        case 0x29: // TwoHandFlail
        case 0x2A: // TwoHandMace
        case 0x2B: // TwoHandGiantMace
        case 0x2E: // TwoHandGiantAxe
        case 0x2F: // TwoHandGiantHammer
        case 0x32: // TwoHandFlamethrower
        case 0x33: // TwoHandIcethrower
        case 0x34: // TwoHandLightningthrower
        case 0x35: // TwoHandBlowPipe
        case 0x37: // TwoHandFlag
            return TransmogSlot::TwoHandWeapon;
        // Gathering tools. The engine files these under their own equip tag rather than with the two-hand weapons,
        // even though the meshes are held in both hands, so they classify as Tool and not TwoHandWeapon.
        //
        // The tool and utility codes shift when the engine enum gains an entry inside their range. An insertion moves
        // ONLY the codes at and above the insertion point, so do not blanket-shift this whole block. Read the current
        // values off the EquipTypeInfo row names, the same way as the weapon codes above.
        //
        // Gaps in the case list are deliberate. Those rows carry hand-held props rather than equipment: baskets,
        // buckets, pots, and the writing set the engine files under Tooltrumpet. Count keeps them out of the picker.
        // The drill reads as a one-hand weapon from its type-code neighbourhood, but the engine files it in the tool
        // slot, so it classifies as Tool for the same reason as the spray rig above.
        case 0x1E: // OneHandDrill
        case 0x5A: // ToolPickaxe
        case 0x5B: // ToolHayfork
        case 0x5C: // ToolRake
        case 0x5D: // ToolAxe
        case 0x5E: // ToolShovel
        case 0x5F: // ToolBroom
        case 0x61: // ToolHoe
        case 0x62: // ToolSythe
        case 0x63: // ToolHammer
        case 0x65: // ToolSaw
        case 0x67: // ToolDrum
        case 0x68: // ToolStick
        case 0x6C: // ToolPriestWandBig
        case 0x6D: // ToolPriestWandSmall
        case 0x6E: // ToolCrutch
            return TransmogSlot::Tool;
        default:
            return TransmogSlot::Count;
        }
    }

    // --- Singleton ---

    ItemNameTable &ItemNameTable::instance()
    {
        static ItemNameTable s;
        return s;
    }

    // Mutex guarding writes to m_idToName/m_nameToId/m_sortedCache during a background-thread publish. Readers
    // (name_of/id_of/sorted_entries) take a shared-ish view via the same mutex. Contention is limited to the handful
    // of picker-popup calls per frame, so a plain std::mutex is fine.
    static std::mutex s_tableMtx;

    // Cached intermediate addresses from the 4-hop chain, resolved once in the first build() call. Keeps retry cost to
    // just the catalog walk (and the null-check on the global holder).
    struct ResolvedChain
    {
        bool resolved = false;
        uintptr_t globalHolder = 0; // address of the iteminfo global pointer holder
        uintptr_t itemAccessor = 0; // ItemAccessor -- IndexedStringA short->hash
    };

    static ResolvedChain &cached_chain()
    {
        static ResolvedChain c;
        return c;
    }

    // Walk SubTranslator -> ... -> iteminfo global pointer holder once and cache the result.
    // Returns false on fatal decoder mismatch (do not retry). On success it fills cached_chain().globalHolder.
    static bool resolve_chain(uintptr_t subTranslatorAddr) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        auto &chain = cached_chain();
        if (chain.resolved)
            return true;

        if (!subTranslatorAddr)
        {
            logger.warning("[nametable] SubTranslator not resolved -- skipping");
            return false;
        }

        // Step 1: locate the call to the item descriptor initializer inside SubTranslator. A fixed offset into the
        // function is not reliable, because compiler prologue reshuffles drift that offset between patches. Scan a
        // bounded 0x80-byte window of the function instead.
        //
        // Two anchor variants are tried, current encoding first:
        //   k_nametableSubTxV105Anchor: 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ??
        //             (mov r8d,1 / lea rdx,[rbp+disp8] / lea rcx,[rsp+disp8])
        //             The second lea is rsp-relative, so it carries a SIB
        //             byte and is one byte longer than the older form.
        //   k_nametableSubTxV104Anchor: 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ??
        //             (mov r8d,1 / lea rdx,[rbp+disp8] / lea rcx,[rbp+disp8])
        //             The second lea is rbp-relative and has no SIB byte.
        //
        // Both disp8 slots are wildcarded so a future stack-frame shift inside the same function does not require
        // another anchor variant. The 0x80-byte window keeps a stray match elsewhere in .text from leaking in.
        const auto subTxStart = reinterpret_cast<const std::byte *>(subTranslatorAddr);

        auto anchorV105 = DMK::Scanner::parse_aob(Transmog::k_nametableSubTxV105Anchor);
        auto anchorV104 = DMK::Scanner::parse_aob(Transmog::k_nametableSubTxV104Anchor);
        if (!anchorV105 || !anchorV104)
        {
            logger.warning("[nametable] parse_aob failed for descriptor-initializer anchors");
            return false;
        }

        const auto *match1 = DMK::Scanner::find_pattern(subTxStart, 0x80, *anchorV105);
        if (!match1)
            match1 = DMK::Scanner::find_pattern(subTxStart, 0x80, *anchorV104);
        if (!match1)
        {
            logger.warning("[nametable] descriptor-initializer call anchor "
                           "not found within the SubTranslator prologue");
            return false;
        }
        // The offset marker `|` points one past the E8, which is the start of disp32.
        // DMK v3.0.2+ applies pattern.offset internally, so do NOT add it again.
        const uintptr_t dispAddr1 = reinterpret_cast<uintptr_t>(match1);
        bool ok = false;
        const auto disp1 = read_i32_safe(dispAddr1, ok);
        if (!ok)
        {
            logger.warning("[nametable] failed to read disp32 at 0x{:X}", dispAddr1);
            return false;
        }
        // RIP-relative target = (end of 5-byte call) + disp32.
        const uintptr_t descInit = (dispAddr1 + 4) + static_cast<intptr_t>(disp1);
        if (!descInit)
            return false;

        // Step 2: first `E8` call inside the item descriptor initializer -> ItemAccessor.
        const uintptr_t itemAccessor = first_rel_call_target(descInit, 0x180);
        if (!itemAccessor)
        {
            logger.warning("[nametable] no rel-call found inside the descriptor initializer");
            return false;
        }

        // Step 3: locate the `mov rbx, cs:<iteminfo holder>` inside ItemAccessor. A fixed offset is not reliable here
        // either, so scan instead. The `48 8B 1D disp32` is preceded by a distinctive 9-byte prologue-tail anchor
        //   41 56 48 83 EC ?? 0F B7 39
        // (push r14 / sub rsp,imm8 / movzx edi,word ptr [rcx]) which pins the specific call site inside a bounded
        // 0x40-byte scan of THIS function. Global uniqueness does not matter, because the scan is locally bounded.
        // The stack-alloc imm8 is wildcarded, because it changes with the frame size (see
        // k_nametableItemAccessorAnchor).
        const auto itemAccessorStart = reinterpret_cast<const std::byte *>(itemAccessor);
        auto anchor3 = DMK::Scanner::parse_aob(Transmog::k_nametableItemAccessorAnchor);
        if (!anchor3)
        {
            logger.warning("[nametable] parse_aob failed for the iteminfo-holder anchor");
            return false;
        }
        const auto *match3 = DMK::Scanner::find_pattern(itemAccessorStart, 0x40, *anchor3);
        if (!match3)
        {
            logger.warning("[nametable] mov-rbx anchor not found within "
                           "the ItemAccessor prologue");
            return false;
        }
        // `|` points at the start of the `48 8B 1D disp32` instruction.
        // DMK v3.0.2+ applies pattern.offset internally, so do NOT add it again.
        const uintptr_t ripInstr = reinterpret_cast<uintptr_t>(match3);
        const auto disp = read_i32_safe(ripInstr + 3, ok);
        if (!ok)
        {
            logger.warning("[nametable] failed to read rip-disp at 0x{:X}", ripInstr + 3);
            return false;
        }
        chain.globalHolder = (ripInstr + 7) + static_cast<intptr_t>(disp);
        chain.itemAccessor = itemAccessor;
        chain.resolved = true;
        logger.info("[nametable] chain resolved: iteminfo holder = 0x{:X}, "
                    "ItemAccessor = 0x{:X}",
                    chain.globalHolder, chain.itemAccessor);
        return true;
    }

    uintptr_t ItemNameTable::indexed_string_lookup_addr() const noexcept
    {
        return cached_chain().itemAccessor;
    }

    ItemNameTable::CatalogInfo ItemNameTable::catalog_info() const noexcept
    {
        CatalogInfo info;
        const auto &chain = cached_chain();
        if (!chain.resolved)
            return info;
        bool ok = false;
        const uintptr_t globalPtr = read_qword_safe(chain.globalHolder, ok);
        if (!ok || globalPtr < 0x10000)
            return info;
        info.count = read_u32_safe(globalPtr + k_iteminfoCountOffset, ok);
        if (!ok || info.count == 0 || info.count > k_maxCatalogSize)
        {
            info.count = 0;
            return info;
        }
        info.ptrArray = read_qword_safe(globalPtr + k_iteminfoPtrArrayOffset, ok);
        if (!ok || info.ptrArray < 0x10000)
        {
            info.ptrArray = 0;
            info.count = 0;
        }
        return info;
    }

    uintptr_t ItemNameTable::descriptor_of(uint16_t itemId) const noexcept
    {
        const auto ci = catalog_info();
        if (ci.ptrArray == 0 || itemId >= ci.count)
            return 0;
        bool ok = false;
        const uintptr_t desc = read_qword_safe(ci.ptrArray + static_cast<uint64_t>(itemId) * 8, ok);
        return (ok && desc > 0x10000) ? desc : 0;
    }

    ItemNameTable::BuildResult ItemNameTable::build(uintptr_t subTranslatorAddr)
    {
        auto &logger = DMK::Logger::get_instance();

        // Step A: resolve and cache the address chain. Fatal on decoder mismatch, because retries do not help.
        if (!resolve_chain(subTranslatorAddr))
            return BuildResult::Fatal;

        const uintptr_t globalHolder = cached_chain().globalHolder;

        // Step B: dereference the holder. Deferred when null, because the game can still be initializing the iteminfo
        // container.
        bool ok = false;
        const uintptr_t globalPtr = read_qword_safe(globalHolder, ok);
        if (!ok || globalPtr < 0x10000)
        {
            logger.trace("[nametable] iteminfo global not initialized "
                         "(holder=0x{:X} value=0x{:X}) -- deferring",
                         globalHolder, globalPtr);
            return BuildResult::Deferred;
        }

        const uint32_t count = read_u32_safe(globalPtr + k_iteminfoCountOffset, ok);
        if (!ok || count == 0 || count > k_maxCatalogSize)
        {
            logger.trace("[nametable] catalog count implausible: {} "
                         "(globalPtr=0x{:X}) -- deferring",
                         count, globalPtr);
            return BuildResult::Deferred;
        }

        const uintptr_t ptrArray = read_qword_safe(globalPtr + k_iteminfoPtrArrayOffset, ok);
        if (!ok || ptrArray < 0x10000)
        {
            logger.trace("[nametable] iteminfo ptrArray null "
                         "(globalPtr=0x{:X} ptrArray=0x{:X}) -- deferring",
                         globalPtr, ptrArray);
            return BuildResult::Deferred;
        }

        logger.info("[nametable] scanning item catalog: count={} "
                    "globalPtr=0x{:X} ptrArray=0x{:X}",
                    count, globalPtr, ptrArray);

        const auto t0 = std::chrono::steady_clock::now();

        // Build into local maps first so the published snapshot is atomic from any reader's viewpoint. Only copy into
        // the member maps under the mutex once walking is done.
        std::unordered_map<uint16_t, std::string> idToName;
        std::unordered_map<std::string, uint16_t> nameToId;
        std::unordered_map<uint16_t, uint8_t> variantFlag;
        std::unordered_map<uint16_t, uint16_t> typeCodeMap;
        idToName.reserve(count);
        nameToId.reserve(count);
        variantFlag.reserve(count);
        typeCodeMap.reserve(count);

        // Pass 1: walk the catalog and collect the name, the variant-meta pointer and the item-type code for every
        // valid descriptor. The variant flag cannot be resolved yet, because pass 2 derives the sentinel
        // statistically from the values this pass collects.
        struct ScratchEntry
        {
            uint16_t id;
            std::string name;
            uintptr_t metaPtr; // 0 on read fault
            uint16_t typeCode; // canonical item-type code
        };
        std::vector<ScratchEntry> scratch;
        scratch.reserve(count);

        std::size_t valid = 0;
        std::size_t collisions = 0;
        char buf[k_maxNameLen + 1];

        for (uint32_t id = 0; id < count; ++id)
        {
            const uintptr_t descPtr = read_qword_safe(ptrArray + id * 8ull, ok);
            if (!ok || !DMKMemory::plausible_userspace_ptr(descPtr))
                continue;

            // descPtr is reused by two downstream reads (metaPtr and typeCode), so it is resolved separately. Only the
            // wrapper to string pointer hops (descPtr -> +0x8 -> +0x0) are folded into one guarded walk.
            const auto strPtrOpt = DMKMemory::seh_read_chain<uintptr_t>(descPtr, {0x8, 0x0});
            if (!strPtrOpt || !DMKMemory::plausible_userspace_ptr(*strPtrOpt))
                continue;
            const uintptr_t strPtr = *strPtrOpt;

            const auto len = read_cstring_safe(strPtr, buf, sizeof(buf));
            if (len == 0 || len >= k_maxNameLen)
                continue;

            const auto id16 = static_cast<uint16_t>(id);
            const uintptr_t metaPtr = read_qword_safe(descPtr + k_descVariantMetaOffset, ok);

            // Wearer-body classification is NOT read here. A game update re-keyed the descriptor rule-classifier body
            // tokens, so the rule list at desc+0x248 no longer yields a usable body class. Body comes from the
            // equip-eligibility ("Male"/"Female") column of the display_names TSV (see load_display_names /
            // m_bodyByName), applied at query time in sorted_entries and is_player_compatible.
            // scripts/gen_item_body_table.py fills that column from the packed gamedata.
            //
            // Item-type code, u16 at k_descTypeCodeOffset:
            //   0x04=Helm, 0x05=Chest, 0x06=Gloves, 0x07=Boots,
            //   0x08=Earring, 0x09=Necklace, 0x0A=Ring, 0x37=Lantern,
            //   0x43=Backpack, 0x46=Cloak, 0x47=Bracelet, 0x48=Glasses, 0x49=Mask,
            //   0x4A..0x53=WarRobot parts, 0x54=Dragon armor,
            //   0xFFFF=Quest/Non-equipment.
            // This is the canonical game-side classifier for item category. Slot derivation does not depend on parsing
            // the item name.
            bool tcOk = false;
            const uint16_t typeCode = read_u16_safe(descPtr + k_descTypeCodeOffset, tcOk);

            scratch.push_back({
                id16,
                std::string(buf, len),
                ok ? metaPtr : 0,
                tcOk ? typeCode : uint16_t{0xFFFF},
            });
            ++valid;
        }

        // Pass 2 -- statistically derive the variant-meta sentinel. The sentinel is the value that appears at
        // desc+k_descVariantMetaOffset in the clear majority of items. Any other pointer at that slot is per-item
        // variant metadata and gates the item out of runtime transmog.
        //
        // Tally the non-zero metaPtr values. The mode is the sentinel.
        uintptr_t resolvedSentinel = 0;
        std::size_t sentinelCount = 0;
        {
            std::unordered_map<uintptr_t, std::size_t> tally;
            tally.reserve(64);
            for (const auto &e : scratch)
            {
                if (e.metaPtr != 0)
                    ++tally[e.metaPtr];
            }
            for (const auto &kv : tally)
            {
                if (kv.second > sentinelCount)
                {
                    sentinelCount = kv.second;
                    resolvedSentinel = kv.first;
                }
            }
            // Require the mode to dominate -- at least 1/3 of valid items must point at it. Below that threshold the
            // data is garbage, and the builder must not flag anything as variant.
            if (valid == 0 || sentinelCount * 3 < valid)
            {
                logger.debug("[nametable] variant sentinel not dominant "
                             "(best=0x{:X} count={}/{}) -- disabling "
                             "variant-meta filter",
                             resolvedSentinel, sentinelCount, valid);
                resolvedSentinel = 0;
            }
            else
            {
                logger.info("[nametable] variant-meta sentinel resolved: "
                            "0x{:X} ({} of {} items)",
                            resolvedSentinel, sentinelCount, valid);
                s_variantMetaSentinel.store(resolvedSentinel, std::memory_order_release);
            }
        }

        // Pass 3 -- publish the scratch rows into the maps and flag variants against the resolved sentinel.
        std::size_t variantCount = 0;
        for (auto &e : scratch)
        {
            // Item "has variant" (picker shows carrier-color) when desc+k_descVariantMetaOffset is non-sentinel. That
            // value is a per-item variant-meta record threaded through the catalog list. Predicting variant as
            // "meta != sentinel" produces no false positives against the previous version's dump, matched BY NAME.
            //
            // A ">= 2 body-bearing classifier rules" heuristic does NOT work as a substitute. The reshaped rule struct
            // gives nearly every item the same large rule count, so that heuristic over-flags and drives the dump
            // correlation from zero false positives to hundreds. The variant-meta pointer alone is the reliable
            // signal. See k_descVariantMetaOffset for how to re-derive the offset.
            const bool hasVariant =
                (resolvedSentinel != 0) && (e.metaPtr != 0) && (e.metaPtr != resolvedSentinel);
            if (hasVariant)
                ++variantCount;

            idToName.emplace(e.id, e.name);
            auto [it, inserted] = nameToId.emplace(std::move(e.name), e.id);
            if (!inserted)
                ++collisions;
            variantFlag.emplace(e.id, hasVariant ? uint8_t{1} : uint8_t{0});
            typeCodeMap.emplace(e.id, e.typeCode);
        }

        // Stability check: the game sets the iteminfo count to its final value early, but it populates the descriptor
        // pointer array lazily. A fixed "good enough" percentage gate is not reliable, because it lets a partial
        // catalog through. Instead, wait until two consecutive scans produce the same valid count, which means the
        // array stopped growing. This self-adapts to any catalog size and any game version.
        if (valid == 0)
        {
            logger.trace("[nametable] no valid descriptors -- deferring");
            m_lastBuildValid = 0;
            return BuildResult::Deferred;
        }
        if (valid != m_lastBuildValid)
        {
            logger.trace("[nametable] catalog still loading "
                         "({} -> {} valid) -- deferring",
                         m_lastBuildValid, valid);
            m_lastBuildValid = static_cast<uint32_t>(valid);
            return BuildResult::Deferred;
        }
        // valid > 0 && valid == m_lastBuildValid -> catalog stabilized.

        // Catalog typeCode histogram. It groups every cataloged item by its desc+k_descTypeCodeOffset typeCode and
        // emits one log line per distinct code: count, current `slot_from_type_code` verdict, and 3 sample item names.
        // The user then sees the FULL universe of typeCodes in one game launch instead of discovering each one by
        // wearing an item. Unmapped codes show with their sample names, so the names identify the slot they belong in
        // (e.g. "samples: Crossbow_Iron_I, ..." => crossbow family => Ranged). Runs once per successful build.
        {
            std::unordered_map<std::uint16_t, std::vector<std::uint16_t>> bucket;
            bucket.reserve(64);
            for (const auto &kv : typeCodeMap)
                bucket[kv.second].push_back(kv.first);

            // Sort typeCodes descending by item count so high-volume codes show first.
            std::vector<std::uint16_t> codes;
            codes.reserve(bucket.size());
            for (const auto &kv : bucket)
                codes.push_back(kv.first);
            std::sort(codes.begin(), codes.end(),
                      [&](std::uint16_t a, std::uint16_t b) { return bucket[a].size() > bucket[b].size(); });

            logger.trace("[catalog-histogram] {} distinct typeCodes across {} items"
                         " (unmapped codes show samples so you can identify them "
                         "and add static slot_from_type_code cases)",
                         codes.size(), valid);

            for (std::uint16_t code : codes)
            {
                auto &ids = bucket[code];
                // Sort itemIds ascending and pick first 3 names for a stable sample window.
                std::sort(ids.begin(), ids.end());
                const auto take = std::min<std::size_t>(3, ids.size());

                std::string samples;
                for (std::size_t k = 0; k < take; ++k)
                {
                    auto it = idToName.find(ids[k]);
                    if (k > 0)
                        samples += ", ";
                    samples += (it != idToName.end()) ? it->second : "<unknown>";
                }

                const TransmogSlot slot = slot_from_type_code(code);
                const char *slotStr = (slot == TransmogSlot::Count) ? "<UNMAPPED>" : slot_name(slot);

                logger.trace("[catalog-histogram]   typeCode={:#06x} count={:>4} "
                             "slot={:<13} samples: {}",
                             code, ids.size(), slotStr, samples);
            }
        }

        {
            std::lock_guard<std::mutex> lk(s_tableMtx);
            m_idToName = std::move(idToName);
            m_nameToId = std::move(nameToId);
            m_variantFlag = std::move(variantFlag);
            m_typeCode = std::move(typeCodeMap);
            m_sortedCache.clear(); // will be rebuilt lazily on next access
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        logger.info("[nametable] built: {}/{} entries ({} name collisions, "
                    "{} variant-meta) in {}ms",
                    valid, count, collisions, variantCount, ms);
        return BuildResult::Ok;
    }

    std::string ItemNameTable::name_of(uint16_t itemId) const
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        auto it = m_idToName.find(itemId);
        if (it == m_idToName.end())
            return {};
        return it->second;
    }

    std::optional<uint16_t> ItemNameTable::id_of(const std::string &name) const
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        auto it = m_nameToId.find(name);
        if (it == m_nameToId.end())
            return std::nullopt;
        return it->second;
    }

    bool ItemNameTable::has_variant_meta(uint16_t itemId) const
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        auto it = m_variantFlag.find(itemId);
        return it != m_variantFlag.end() && it->second != 0;
    }

    bool ItemNameTable::is_player_compatible(uint16_t itemId) const
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        // Kliff-centric: safe to bind on the (male) player unless the item is restricted to the female body. Body is
        // sourced from the display_names equip-eligibility column (m_bodyByName, keyed by lowercase internal name).
        auto nit = m_idToName.find(itemId);
        if (nit == m_idToName.end())
            return true; // unknown -> prefer to surface
        std::string key = nit->second;
        for (auto &c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto bit = m_bodyByName.find(key);
        return bit == m_bodyByName.end() || bit->second != BodyKind::Female;
    }

    ItemNameTable::BodyKind ItemNameTable::body_kind_for_item(uint16_t itemId) const
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        // Lowercase the internal name to match m_bodyByName's keys. An item wearable by both bodies (or
        // unrestricted) is absent from the map and resolves to BodyKind::Generic.
        auto nit = m_idToName.find(itemId);
        if (nit == m_idToName.end())
            return BodyKind::Generic;
        std::string key = nit->second;
        for (auto &c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto bit = m_bodyByName.find(key);
        return bit != m_bodyByName.end() ? bit->second : BodyKind::Generic;
    }

    TransmogSlot ItemNameTable::category_of(uint16_t itemId) const noexcept
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        // Runtime-observed binding wins. If the engine actually equipped this itemId in a slot, that beats the static
        // type-code heuristic. Accessory and weapon items then show up in the correct picker the moment the
        // auth-table reveals them, even when `slot_from_type_code` does not know their typeCode yet.
        if (auto obs = m_observedSlot.find(itemId); obs != m_observedSlot.end())
            return obs->second;

        auto it = m_typeCode.find(itemId);
        if (it == m_typeCode.end())
            return TransmogSlot::Count;
        return slot_from_type_code(it->second);
    }

    std::uint16_t ItemNameTable::type_code_of(std::uint16_t itemId) const noexcept
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        auto it = m_typeCode.find(itemId);
        if (it == m_typeCode.end())
            return 0xFFFFu;
        return it->second;
    }

    void ItemNameTable::record_observed_slot(std::uint16_t itemId, TransmogSlot slot) noexcept
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        if (slot == TransmogSlot::Count)
        {
            m_observedSlot.erase(itemId);
            return;
        }
        m_observedSlot[itemId] = slot;
    }

    std::size_t ItemNameTable::observed_slot_count() const noexcept
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        return m_observedSlot.size();
    }

    ItemNameTable::BodyKind ItemNameTable::body_kind_for_character(const std::string &charName) noexcept
    {
        // Fixed per-character body: Kliff and Oongka use the male humanoid skeleton, Damiane the female one. Unknown
        // characters default to Generic. Their body is not known, so treat every item as potentially compatible
        // rather than silently hiding their picker.
        if (charName == "Kliff" || charName == "Oongka")
            return BodyKind::Male;
        if (charName == "Damiane")
            return BodyKind::Female;
        return BodyKind::Generic;
    }

    const std::vector<ItemNameTable::Entry> &ItemNameTable::sorted_entries() const
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        if (!m_sortedCache.empty() || m_idToName.empty())
            return m_sortedCache;

        m_sortedCache.reserve(m_idToName.size());
        for (const auto &[id, name] : m_idToName)
        {
            auto vit = m_variantFlag.find(id);
            const bool hasVariant = (vit != m_variantFlag.end()) && (vit->second != 0);

            // Lowercased internal name keys both the display-name and the wearer-body maps (both loaded from the
            // display_names TSV).
            std::string lowerName = name;
            for (auto &c : lowerName)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // Wearer-body classification comes from the equip-eligibility column of the display_names TSV
            // (m_bodyByName). Only single-body-restricted items are listed. Anything wearable by both bodies (or
            // unrestricted) is absent -> BodyKind::Generic, shown on every character. This replaces the
            // rule-classifier token machinery, which a game update re-keyed. See scripts/gen_item_body_table.py for
            // how the column is filled.
            auto brit = m_bodyByName.find(lowerName);
            const BodyKind kind = (brit != m_bodyByName.end()) ? brit->second : BodyKind::Generic;
            // Kliff-centric "PlayerSafe": an item is player-safe unless it is restricted to the female body.
            const bool isPlayer = (kind != BodyKind::Female);

            auto dit = m_displayNames.find(lowerName);
            std::string dispName = (dit != m_displayNames.end()) ? dit->second : std::string();

            // The canonical item-type code is authoritative. Unmapped codes (pet and mount armor, WarRobot parts,
            // 0xFFFF quest items and so on) map to Count, so the item is hidden as non-equipment. An absent type-code
            // entry also collapses to Count. There is no name-parsing fallback, because the engine itself reads this
            // field to categorize the item.
            TransmogSlot slot = TransmogSlot::Count;
            auto tcit = m_typeCode.find(id);
            if (tcit != m_typeCode.end())
                slot = slot_from_type_code(tcit->second);

            m_sortedCache.push_back({
                id,
                slot,
                hasVariant,
                isPlayer,
                kind,
                name,
                std::move(dispName),
            });
        }

        std::sort(m_sortedCache.begin(), m_sortedCache.end(),
                  [](const Entry &a, const Entry &b)
                  {
                      // Sort by display name when available, else by internal name. Case-insensitive so "Kliff" and
                      // "kliff" sort together.
                      const auto &sa = a.displayName.empty() ? a.name : a.displayName;
                      const auto &sb = b.displayName.empty() ? b.name : b.displayName;
                      const std::size_t n = (std::min)(sa.size(), sb.size());
                      for (std::size_t i = 0; i < n; ++i)
                      {
                          const auto ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(sa[i])));
                          const auto cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(sb[i])));
                          if (ca != cb)
                              return ca < cb;
                      }
                      return sa.size() < sb.size();
                  });

        return m_sortedCache;
    }

    void ItemNameTable::dump_catalog_tsv() const
    {
        auto &logger = DMK::Logger::get_instance();

        std::wstring rtDir = DMK::Filesystem::get_runtime_directory();
        if (rtDir.empty())
        {
            logger.warning("[nametable] dump_catalog_tsv: runtime dir unavailable");
            return;
        }
        if (rtDir.back() != L'\\' && rtDir.back() != L'/')
            rtDir.push_back(L'\\');

        std::wstring path = rtDir + L"CrimsonDesertLiveTransmog_items.tsv";

        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            logger.warning("[nametable] dump_catalog_tsv: failed to open output file");
            return;
        }

        const auto &entries = sorted_entries();

        out << "ItemID\tSlot\tVariant\tPlayerSafe\tName\n";
        for (const auto &e : entries)
        {
            const char *slotStr = "Other";
            if (e.category != TransmogSlot::Count)
                slotStr = slot_name(e.category);

            out << "0x" << std::hex << std::uppercase << e.id << std::dec << '\t' << slotStr << '\t'
                << (e.hasVariantMeta ? "yes" : "no") << '\t' << (e.isPlayerCompatible ? "yes" : "no") << '\t' << e.name
                << '\n';
        }

        logger.info("[nametable] dumped {} entries to CrimsonDesertLiveTransmog_items.tsv", entries.size());
    }

    void ItemNameTable::load_display_names(const std::string &tsvPath)
    {
        auto &logger = DMK::Logger::get_instance();

        std::ifstream file(tsvPath);
        if (!file.is_open())
        {
            logger.warning("[nametable] display names file not found: '{}'", tsvPath);
            return;
        }

        std::unordered_map<std::string, std::string> names;
        std::unordered_map<std::string, BodyKind> bodyByName;
        names.reserve(6100);
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            if (line.back() == '\r')
                line.pop_back();

            const auto t1 = line.find('\t');
            if (t1 == std::string::npos || t1 == 0)
                continue;

            std::string key = line.substr(0, t1);
            for (auto &c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // Columns: <internal name> \t <display name> [\t <wearer body: "Male"|"Female">]. The optional 3rd column
            // carries the equip-eligibility body restriction (scripts/gen_item_body_table.py fills it from the packed
            // gamedata) and is only present for single-body-restricted items, so an older 2-column TSV still loads --
            // absent body just means "unrestricted / shown on every character".
            std::string display, body;
            const auto t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos)
                display = line.substr(t1 + 1);
            else
            {
                display = line.substr(t1 + 1, t2 - (t1 + 1));
                body = line.substr(t2 + 1);
            }
            if (body == "Male")
                bodyByName.emplace(key, BodyKind::Male);
            else if (body == "Female")
                bodyByName.emplace(key, BodyKind::Female);
            if (!display.empty())
                names.emplace(std::move(key), std::move(display));
        }

        {
            std::lock_guard<std::mutex> lk(s_tableMtx);
            m_displayNames = std::move(names);
            m_bodyByName = std::move(bodyByName);
            // Callers must invoke load_display_names() before any sorted_entries() access (i.e. before
            // dump_catalog_tsv) so the cache is still empty here -- no re-sort needed.
            m_sortedCache.clear();
        }

        logger.info("[nametable] loaded {} display names ({} body-restricted) from '{}'", m_displayNames.size(),
                    m_bodyByName.size(), tsvPath);
    }

    std::string ItemNameTable::display_name_of(std::string_view internalName) const
    {
        // Lowercase into a stack buffer to avoid heap allocation. Item names in the catalog are bounded by
        // k_maxNameLen, which is smaller than this buffer, and the copy is clamped to the buffer size anyway.
        char buf[256];
        const auto len = (std::min)(internalName.size(), sizeof(buf) - 1);
        for (std::size_t i = 0; i < len; ++i)
            buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(internalName[i])));
        buf[len] = '\0';
        const std::string key{buf, len};

        std::lock_guard<std::mutex> lk(s_tableMtx);
        const auto it = m_displayNames.find(key);
        if (it == m_displayNames.end())
            return {};
        return it->second;
    }

} // namespace Transmog
