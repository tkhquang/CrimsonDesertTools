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
    // Sensible upper bound on item descriptor catalog size. v1.02.00 ships ~6024 entries; guard generously.
    static constexpr uint32_t k_maxCatalogSize = 0x20000;

    static constexpr std::size_t k_maxNameLen = 96;

    // Variant-metadata detection (see item_name_table.hpp::has_variant_meta). Clean base items have `*(desc+<offset>)
    // == <sentinel>` where <sentinel> is a shared empty-object pointer (IDA: off_1459D0B38 on v1.02.00 -- an
    // IRefCounted vtable in the exe's .data section). Non-sentinel values point to a per-item metadata struct threaded
    // through a catalog-wide linked list; members of this list failed to render via runtime transmog on all tested
    // samples.
    //
    // The field offset shifts across major game revisions as the descriptor struct grows:
    //
    //   v1.02.00 -- +0x3A0 (original)
    //   v1.04.00 -- +0x3C8 (descriptor gained 0x28 bytes of new fields
    //               between +0x3A0 and +0x3C8; +0x3A0 now carries an
    //               unrelated `0x1FFFFFFFF` bit-pattern value)
    //   v1.05.00 -- +0x3D0 (descriptor gained another 8 bytes;
    //               desc+0x298 also holds a sentinel on every item, so
    //               the constraint that distinguishes the variant-meta
    //               field is "sentinel for direct-wear items, per-item
    //               heap pointer for carrier-required items"). Anchor
    //               sentinel observed at 0x145BC3638.
    //   v1.13.00 -- +0x3B0 (this region of the descriptor SHRANK 0x20 --
    //               old +0x3D0 now reads a constant 0 on every item). The
    //               variant-meta pointer sits 0x20 lower. Re-derived by
    //               probing every descriptor qword in 0x380..0x480 and
    //               correlating "value != mode" against the v1.12.00 dump's
    //               Variant column BY NAME: +0x3B0 gave fp=0, acc=98.1%
    //               (sentinel 0x145E49F98, 60% of items). No other offset
    //               came close.
    //
    // The sentinel value itself is also unstable (any .data reshuffle moves it). Rather than hardcoding either the
    // offset or the sentinel RVA, the builder resolves the sentinel statistically at catalog-build time: scan every
    // valid descriptor's qword at this offset, the value that appears in the clear majority of items (~3-5k of ~6k) IS
    // the sentinel. Self-heals across future game updates as long as the catalog stays statistically dominated by base
    // items.
    static constexpr std::ptrdiff_t k_descVariantMetaOffset = 0x3B0;

    // --- iteminfo (qword_145CEF370) container layout, v1.02.00 ---
    // Source: IDA sub_1402D75D0 + static analysis. These are runtime data offsets (not code) so they cannot be
    // AOB-scanned; if a future patch reshapes the struct the catalog walk will produce an implausible count/ptrArray
    // and bail at the existing sanity checks below.
    static constexpr std::ptrdiff_t k_iteminfoCountOffset = 0x08;    // dword entry count
    static constexpr std::ptrdiff_t k_iteminfoPtrArrayOffset = 0x50; // 80: qword base of descriptor ptr array

    // Resolved sentinel, cached after first successful build(). 0 means "not yet resolved" -- has_variant_meta() falls
    // back to false (preferring to let a bad item through rather than mis-flag a clean one) until the next build()
    // populates it.
    static std::atomic<uintptr_t> s_variantMetaSentinel{0};

    // --- Safe memory helpers ---
    //
    // The `(value, bool& ok)` shape distinguishes a faulted read from a legitimate zero result, which matters at call
    // sites where 0 is a valid value (e.g. slot index 0 versus unread slot field). `DMKMemory::seh_read<T>` is the
    // underlying SEH-protected primitive; these adapters fold its `std::optional<T>` return into the local shape used
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
                // the sequence `E2 85 A2..A5`), so a printable-ASCII-only filter would silently drop them.
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
    // Driven entirely by the canonical item-type code at desc+0x44 as captured during the catalog build. Name-parsing
    // heuristics have been removed -- they produced false positives on non-armor items whose names happened to contain
    // tokens like "_Armor_" (horse armors, shields, quest treasure maps, etc.). The game's own type code is
    // unambiguous; the fallback is to treat anything unmapped as non-equipment.

    // Slot mapping for the canonical item-type code at desc+0x44. Helm/Chest/Gloves/Boots are stable across
    // v1.03.01/v1.04.00. Cloak shifted from 0x45 to 0x46 on v1.04.00 (a one-code insertion in the engine enum between
    // Boots and Cloak). Both the legacy and current values are accepted so the mapping survives patches that have not
    // shifted the earlier slots.
    //
    // Armor block (existing):
    //   0x04=Helm  0x05=Chest  0x06=Gloves  0x07=Boots
    //   0x45/0x46=Cloak (shifted +1 on v1.04.00)
    //
    // Accessories:
    //   0x08=Earring (Earring1/Earring2 share)
    //   0x09=Necklace
    //   0x0A=Ring (Ring1/Ring2 share)
    //   0x37=Lantern
    //   0x43=Backpack
    //   0x47=Bracelet  0x48=Glasses  0x49=Mask
    //
    // Weapons (typeCode varies by weapon FAMILY and sometimes by character variant -- all sharing the
    // MainHand/OffHand/Ranged/ SubWeapon/TwoHandWeapon auth-table slots):
    //   Weapon families were derived by iterating every descriptor's
    //   typeCode against sample item names. Live-equipped items provided
    //   ground truth for confirmed entries; the rest were classified from
    //   sample-name conventions (e.g. *_TwoHandHammer = TwoHandWeapon,
    //   *_OneHandShotgun = Ranged). Items where the slot couldn't be
    //   inferred from the name were left unmapped -- runtime
    //   observation handles them when the user actually equips one.
    //
    //   1H families (MainHand, paired-pickered with OffHand):
    //     0x00=1H Sword            0x01=OneHandShield (generic)
    //     0x02=Damiane Shield      0x10=1H Axe
    //     0x13=1H Mace             0x1D=Fist (Item_Fist_*)
    //     0x20=Tower Shield        0x27=Rapier
    //     0x30=Knuckle/Ring_Drill
    //   Ranged:
    //     0x03=Bow                 0x0D=Sprayer/utility (named BackPack)
    //     0x22=Pistol              0x23=Musket
    //     0x24=Shotgun             0x25=1H Cannon
    //     0x2F=Crossbow            0x31=FlameThrower
    //     0x32=IceThrower          0x33=LightningThrower
    //     0x64=FishingRod
    //   Sub-weapons:
    //     0x0E=Dagger
    //   2H weapons (combat + utility tools):
    //     0x0F=2H Axe              0x11=Greatsword A
    //     0x12=Greatsword B (NPC)  0x14=WarHammer
    //     0x15=2H Spear/Polearm    0x1C=2H Hammer
    //     0x1E=Drill               0x1F=Chainsaw
    //     0x21=Halberd/Alebard     0x26=2H Cannon
    //     0x55=Pickaxe             0x56=Iron Chain
    //     0x57=Rake                0x58=Felling Axe (Boss_Reward_SuperWeapon)
    //     0x59=Shovel              0x5A=Broom
    //     0x5B=Hoe                 0x5C=Sickle/Scythe
    //     0x5D=Work Hammer         0x5F=Saw
    //     0x62=Stick               0x66=PriestWand
    //     0x68=Crutch
    //
    //   Intentionally NOT mapped (not character transmog):
    //     0x0B Oongka_Rocket_Helm (TransmogSlot::OongkaRocket commented)
    //     0x17 Contribution_Flag, 0x18 Torch, 0x2B Witch_WingFan,
    //     0x34 Poison_Stick (ambiguous; let runtime obs handle)
    //     0x3A-0x3C PetArmor (pet/cat equipment)
    //     0x3D-0x41 HorseArmor (mount equipment)
    //     0x4A-0x54 WarRobot body parts (mech, not character)
    //     0x60 Notepad/Pen, 0x6A FlatBasket, 0x6B Bucket, 0x6D Pot_Head
    //     0xFFFF Arrow/Quiver/Quest items
    //
    // The 0x11 vs 0x12 split for 2H bastard swords looks like a character/gender variant axis -- they're the "same"
    // weapon class visually but the engine tags them differently per protagonist. Both go in the TwoHandWeapon slot.
    //
    // More weapon-family typeCodes will surface as users equip new classes (crossbow, polearm, etc). Runtime
    // `record_observed_slot` covers any unmapped family automatically once an item appears in the auth-table.
    //
    // Excluded by design (not LT-targeted):
    //   0x0B=OongkaRocket Helm (the OongkaRocket TransmogSlot is not
    //        mapped; leaving 0x0B unmapped keeps those items out of
    //        every picker).
    //
    // For paired slots (weapons/earrings/rings) the static map points at the lower-indexed half of the pair; the picker
    // UI uses `slots_share_picker` so both halves of a pair show the same items. The actual auth-table slot used at
    // apply time is whichever TransmogSlot row the user committed against.
    //
    // Other observed codes explicitly rejected:
    //   0x20=Shield, 0x53/0x54=Horse/mount armor, 0xFFFF=Quest/non-equipment.
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
        // The accessory typeCode range (Backpack..Mask) took a uniform +1 shift on v1.13.00 -- the game inserted a new
        // type code at the bottom of the range and pushed everything after it up by one, exactly as v1.06.00 did once
        // already. Verified live on 1.13 (build 1.0.0.1929): Cloak reads 0x48 (Mercenary_Leather_Cloak,
        // Ashad_PlateArmor_Cloak), Backpack reads 0x45 (Fish_BackPack); the rest follow the same +1. Cross-checked by
        // diffing the v1.12.00 vs v1.13.00 item dumps by name -- every accessory moved one slot up (Cloak->Bracelet,
        // Backpack->Cloak, Bracelet->Glasses, Glasses->Mask, Mask->Other under the stale mapping). Armor (Helm..Boots
        // 0x04..0x07) and Earring/Necklace/Ring/Lantern sit below the shifted range and are unchanged.
        //
        // Pre-v1.13 values are intentionally dropped rather than kept for back-compat: they collide with the new ones
        // (old Cloak 0x45 == new Backpack 0x45, old Bracelet 0x48 == new Cloak 0x48, ...), and the auto-updating live
        // game only ever presents the current build's codes. Re-derive from a fresh dump diff if a future patch shifts
        // the range again.
        case 0x46:
        case 0x47:
        case 0x48:
            return TransmogSlot::Cloak;
        case 0x08:
            return TransmogSlot::Earring1; // shared with Earring2
        case 0x09:
            return TransmogSlot::Necklace;
        case 0x0A:
            return TransmogSlot::Ring1; // shared with Ring2
        case 0x37:
            return TransmogSlot::Lantern;
        case 0x44:
        case 0x45:
            return TransmogSlot::Backpack;
        case 0x49:
            return TransmogSlot::Bracelet;
        case 0x4A:
            return TransmogSlot::Glasses;
        case 0x4B:
            return TransmogSlot::Mask;
        // 1H weapons -- all share MainHand (paired with OffHand)
        case 0x00: // 1H sword
        case 0x01: // shield (generic)
        case 0x02: // shield (Damiane variant)
        case 0x10: // 1H axe
        case 0x13: // 1H mace
        case 0x1D: // fist (Item_Fist_*)
        case 0x20: // tower shield
        case 0x27: // rapier
        case 0x30:
            return TransmogSlot::MainHand; // knuckle / ring drill
        // Ranged
        case 0x03: // bow
        case 0x0D: // sprayer/utility ranged
        case 0x22: // pistol
        case 0x23: // musket
        case 0x24: // shotgun
        case 0x25: // 1H cannon
        case 0x2F: // crossbow
        case 0x31: // flamethrower
        case 0x32: // ice thrower
        case 0x33: // lightning thrower
        case 0x64:
            return TransmogSlot::Ranged; // fishing rod
        // Sub-weapons
        case 0x0E:
            return TransmogSlot::SubWeapon; // dagger
        // 2H weapons (combat + utility tools)
        case 0x0F: // 2H axe
        case 0x11: // 2H greatsword A
        case 0x12: // 2H greatsword B (Damiane/NPC)
        case 0x14: // war hammer
        case 0x15: // 2H spear / polearm
        case 0x1C: // 2H hammer
        case 0x1E: // drill
        case 0x1F: // chainsaw
        case 0x21: // halberd / alebard
        case 0x26: // 2H cannon
        case 0x55: // pickaxe
        case 0x56: // iron chain
        case 0x57: // rake
        case 0x58: // felling axe / boss super weapon
        case 0x59: // shovel
        case 0x5A: // broom
        case 0x5B: // hoe
        case 0x5C: // sickle/scythe
        case 0x5D: // work hammer
        case 0x5F: // saw
        case 0x62: // stick
        case 0x66: // priest wand
        case 0x68:
            return TransmogSlot::TwoHandWeapon; // crutch
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
    // (name_of/id_of/sorted_entries) take a shared-ish view via the same mutex; build contention is limited to the
    // handful of picker-popup calls per frame, so a plain std::mutex is fine.
    static std::mutex s_tableMtx;

    // Cached intermediate addresses from the 4-hop chain, resolved once in the first build() call. Keeps retry cost to
    // just the catalog walk (and the null-check on the global holder).
    struct ResolvedChain
    {
        bool resolved = false;
        uintptr_t globalHolder = 0; // &qword_145CEF370
        uintptr_t itemAccessor = 0; // sub_1402D75D0 -- IndexedStringA short->hash
    };

    static ResolvedChain &cached_chain()
    {
        static ResolvedChain c;
        return c;
    }

    // Walk sub_14076D950 -> ... -> qword_145CEF370 address once and cache.
    // Returns false on fatal decoder mismatch (do not retry). On success fills cached_chain().globalHolder.
    static bool resolve_chain(uintptr_t subTranslatorAddr) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        auto &chain = cached_chain();
        if (chain.resolved)
            return true;

        if (!subTranslatorAddr)
        {
            logger.warning("[nametable] sub_14076D950 not resolved -- skipping");
            return false;
        }

        // Step 1: locate the `call sub_141D45270` inside sub_14076D950. Historically at fixed offset +0x2A, but
        // compiler prologue reshuffles drift that offset between patches. Scan a bounded 0x80-byte window of the
        // function instead.
        //
        // Two anchor variants are tried (v1.05.00 first, v1.04.00 fallback):
        //   v1.05.00: 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ??
        //             (mov r8d,1 / lea rdx,[rbp+disp8] / lea rcx,[rsp+disp8])
        //             The second lea encodes rsp-relative (4 bytes)
        //             instead of v1.04's rbp-relative (3 bytes); both
        //             disp8 values are also reshuffled. 1 unique hit on
        //             v1.05.00 at 0x140799CB9 inside SubTranslator.
        //   v1.04.00: 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ??
        //             (mov r8d,1 / lea rdx,[rbp+disp8] / lea rcx,[rbp+disp8])
        //
        // Both disp8 slots are wildcarded so a future stack-frame shift inside the same function does not require
        // another anchor variant. The 0x80-byte window keeps a stray match elsewhere in .text from leaking in.
        const auto subTxStart = reinterpret_cast<const std::byte *>(subTranslatorAddr);

        auto anchorV105 = DMK::Scanner::parse_aob(Transmog::k_nametableSubTxV105Anchor);
        auto anchorV104 = DMK::Scanner::parse_aob(Transmog::k_nametableSubTxV104Anchor);
        if (!anchorV105 || !anchorV104)
        {
            logger.warning("[nametable] parse_aob failed for sub_141D45270 anchors");
            return false;
        }

        const auto *match1 = DMK::Scanner::find_pattern(subTxStart, 0x80, *anchorV105);
        if (!match1)
            match1 = DMK::Scanner::find_pattern(subTxStart, 0x80, *anchorV104);
        if (!match1)
        {
            logger.warning("[nametable] anchor for sub_141D45270 call "
                           "not found within sub_14076D950 prologue");
            return false;
        }
        // offset marker `|` points one past the E8 = start of disp32.
        // DMK v3.0.2+ applies pattern.offset internally; do NOT add it again.
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

        // Step 2: first `E8` call inside sub_141D45270 -> sub_1402D75D0.
        const uintptr_t itemAccessor = first_rel_call_target(descInit, 0x180);
        if (!itemAccessor)
        {
            logger.warning("[nametable] no rel-call found inside sub_141D45270");
            return false;
        }

        // Step 3: locate the `mov rbx, cs:qword_145CEF370` inside sub_1402D75D0. Historically at fixed offset +0x15.
        // Scan instead:
        // the `48 8B 1D disp32` is preceded by a distinctive 6-byte prologue-tail anchor
        //   41 56 48 83 EC 40 0F B7 39
        // (push r14 / sub rsp,40h / movzx edi,word ptr [rcx]) which pins the specific call site inside a bounded
        // 0x40-byte scan of THIS function -- global uniqueness doesn't matter, the scan is locally bounded.
        const auto itemAccessorStart = reinterpret_cast<const std::byte *>(itemAccessor);
        auto anchor3 = DMK::Scanner::parse_aob(Transmog::k_nametableItemAccessorAnchor);
        if (!anchor3)
        {
            logger.warning("[nametable] parse_aob failed for qword_145CEF370 anchor");
            return false;
        }
        const auto *match3 = DMK::Scanner::find_pattern(itemAccessorStart, 0x40, *anchor3);
        if (!match3)
        {
            logger.warning("[nametable] mov-rbx anchor not found within "
                           "sub_1402D75D0 prologue");
            return false;
        }
        // `|` points at start of `48 8B 1D disp32` instruction.
        // DMK v3.0.2+ applies pattern.offset internally; do NOT add it again.
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
        logger.info("[nametable] chain resolved: qword_145CEF370 holder = 0x{:X}, "
                    "sub_1402D75D0 = 0x{:X}",
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

        // Step A: resolve and cache the address chain. Fatal on decoder mismatch -- retries won't help.
        if (!resolve_chain(subTranslatorAddr))
            return BuildResult::Fatal;

        const uintptr_t globalHolder = cached_chain().globalHolder;

        // Step B: dereference the holder. Deferred when null -- the game may not have initialized the iteminfo
        // container yet.
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
        std::unordered_map<uint16_t, uint16_t> equipTypeMap;
        std::unordered_map<uint16_t, uint16_t> typeCodeMap;
        idToName.reserve(count);
        nameToId.reserve(count);
        variantFlag.reserve(count);
        equipTypeMap.reserve(count);
        typeCodeMap.reserve(count);

        // Pass 1: walk the catalog, collect names + variant-meta pointers
        // + player-classifier flag for every valid descriptor. Variant flag
        // can't be resolved yet (sentinel is derived statistically in pass 2), but the player-classifier check is
        // self-contained per item.
        struct ScratchEntry
        {
            uint16_t id;
            std::string name;
            uintptr_t metaPtr;  // 0 on read fault
            uint16_t equipType; // raw u16 at desc+0x42, 0 on read fault
            uint16_t typeCode;  // desc+0x44 -- canonical item-type code
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

            // descPtr is reused by three downstream reads (metaPtr, equipType@+0x42, typeCode@+0x44), so it is resolved
            // separately; only the wrapper -> string pointer hops (descPtr -> +0x8 -> +0x0) are folded into one guarded
            // walk.
            const auto strPtrOpt = DMKMemory::seh_read_chain<uintptr_t>(descPtr, {0x8, 0x0});
            if (!strPtrOpt || !DMKMemory::plausible_userspace_ptr(*strPtrOpt))
                continue;
            const uintptr_t strPtr = *strPtrOpt;

            const auto len = read_cstring_safe(strPtr, buf, sizeof(buf));
            if (len == 0 || len >= k_maxNameLen)
                continue;

            const auto id16 = static_cast<uint16_t>(id);
            const uintptr_t metaPtr = read_qword_safe(descPtr + k_descVariantMetaOffset, ok);

            // Wearer-body classification is NOT read here: the 1.13 game re-keyed the descriptor rule-classifier body
            // tokens (the rule list at +0x248 no longer yields a usable body class), so body now comes from the
            // equip-eligibility ("Male"/"Female") column of the display_names TSV (see load_display_names / m_bodyByName),
            // applied at query time in sorted_entries and is_player_compatible. scripts/gen_item_body_table.py fills that
            // column from the packed gamedata.
            bool etOk = false;
            const uint16_t equipType = read_u16_safe(descPtr + 0x42, etOk);

            // Item-type code at desc+0x44 (u16):
            //   0x04=Helm, 0x05=Chest, 0x06=Gloves, 0x07=Boots,
            //   0x45/0x46=Cloak (shifted +1 on v1.04.00),
            //   0x20=Shield, 0x53/0x54=Horse/mount armor,
            //   0xFFFF=Quest/Non-equipment.
            // This is the canonical game-side classifier for item category; slot derivation no longer depends on
            // parsing the item name.
            bool tcOk = false;
            const uint16_t typeCode = read_u16_safe(descPtr + 0x44, tcOk);

            scratch.push_back({
                id16,
                std::string(buf, len),
                ok ? metaPtr : 0,
                etOk ? equipType : uint16_t{0},
                tcOk ? typeCode : uint16_t{0xFFFF},
            });
            ++valid;
        }

        // Pass 2 -- statistically derive the variant-meta sentinel. The sentinel is the value that appears at
        // desc+k_descVariantMetaOffset in the clear majority of items (historically ~60% on v1.02.00). Any other
        // pointer at that slot = per-item variant metadata and gates out of runtime transmog.
        //
        // Tally non-zero metaPtr values; the mode is the sentinel.
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
            // Require the mode to dominate -- at least 1/3 of valid items must point at it -- otherwise we're looking
            // at garbage and should not flag anything as variant.
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

        // Pass 3 -- publish the scratch rows into the maps, flagging variants against the resolved sentinel + the
        // player-classifier verdict captured in pass 1.
        std::size_t variantCount = 0;
        for (auto &e : scratch)
        {
            // Item "has variant" (picker shows carrier-color) when
            // desc+k_descVariantMetaOffset is non-sentinel -- a per-item variant-meta record threaded through the
            // catalog list. Validated on 1.13.00 (offset 0x3B0): predicting variant = "meta != sentinel" scores fp=0 /
            // acc=98.1% against the v1.12.00 dump BY NAME.
            //
            // A ">= 2 body-bearing classifier rules" heuristic was considered and rejected on 1.13.00: the reshaped
            // rule struct gives nearly every item ~11 rules, so that heuristic over-flags (dump correlation moved from
            // 0 to 864 false positives). The variant-meta pointer alone is the reliable signal. See the
            // k_descVariantMetaOffset history for the offset derivation.
            const bool hasVariant =
                (resolvedSentinel != 0) && (e.metaPtr != 0) && (e.metaPtr != resolvedSentinel);
            if (hasVariant)
                ++variantCount;

            idToName.emplace(e.id, e.name);
            auto [it, inserted] = nameToId.emplace(std::move(e.name), e.id);
            if (!inserted)
                ++collisions;
            variantFlag.emplace(e.id, hasVariant ? uint8_t{1} : uint8_t{0});
            if (e.equipType != 0)
                equipTypeMap.emplace(e.id, e.equipType);
            typeCodeMap.emplace(e.id, e.typeCode);
        }

        // Stability check: the game sets the iteminfo count to its final value (6024 on v1.02/v1.03) early but
        // populates the descriptor pointer array lazily. Instead of comparing against a magic "good enough" threshold
        // (the old 50% gate let a 3432/6024 partial catalog through on v1.03.01), we wait until two consecutive scans
        // produce the same valid count -- meaning the array has stopped growing. This self-adapts to any catalog size
        // and any game version.
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

        // Catalog typeCode histogram. Groups every cataloged item by its `desc+0x44` typeCode and emits one log line
        // per distinct code: count, current `slot_from_type_code` verdict, and 3 sample item names. Lets the user see
        // the FULL universe of typeCodes in one game launch instead of having to discover each one by wearing an item
        // -- unmapped codes show with their sample names so it's obvious from the names which slot they belong in (e.g.
        // "samples: Crossbow_Iron_I, ..." => crossbow family => Ranged). Runs once per successful build.
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
            m_equipType = std::move(equipTypeMap);
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
        // Lowercase the internal name to match m_bodyByName's keys; an item wearable by both bodies (or
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

    uint16_t ItemNameTable::equip_type_of(uint16_t itemId) const noexcept
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        auto it = m_equipType.find(itemId);
        return (it != m_equipType.end()) ? it->second : uint16_t{0};
    }

    TransmogSlot ItemNameTable::category_of(uint16_t itemId) const noexcept
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        // Runtime-observed binding wins -- if the engine has actually equipped this itemId in a slot, we trust that
        // over the static type-code heuristic. Lets accessory/weapon items show up in the correct picker the moment
        // they're seen in the auth-table, even if `slot_from_type_code` doesn't know their typeCode yet.
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
        // characters default to Generic -- we don't know their body, so treat every item as potentially compatible
        // rather than silently hiding their picker.
        if (charName == "Kliff" || charName == "Oongka")
            return BodyKind::Male;
        if (charName == "Damiane")
            return BodyKind::Female;
        return BodyKind::Generic;
    }

    bool ItemNameTable::has_npc_equip_type(uint16_t itemId) const noexcept
    {
        // Player (Kliff) equip-type u16 at desc+0x42 is 0x0004. NPC items use 0x0001. Any value != 0x0004 needs the
        // carrier path so the pipeline sees a Kliff-compatible descriptor.
        static constexpr std::ptrdiff_t k_equipTypeOffset = 0x42;
        static constexpr std::uint16_t k_playerEquipType = 0x0004;

        const auto desc = descriptor_of(itemId);
        if (desc == 0)
            return false; // can't read -- default to direct apply

        bool ok = false;
        const auto eType = read_u16_safe(desc + k_equipTypeOffset, ok);
        return ok && eType != k_playerEquipType;
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
            auto eit = m_equipType.find(id);
            const uint16_t equipT = (eit != m_equipType.end()) ? eit->second : uint16_t{0};

            // Lowercased internal name keys both the display-name and the wearer-body maps (both loaded from the
            // display_names TSV).
            std::string lowerName = name;
            for (auto &c : lowerName)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // Wearer-body classification comes from the equip-eligibility column of the display_names TSV
            // (m_bodyByName). Only single-body-restricted items are listed; anything wearable by both bodies (or
            // unrestricted) is absent -> BodyKind::Generic, shown on every character. This supersedes the old
            // rule-classifier token machinery, which the 1.13 game update rendered inert. See
            // scripts/gen_item_body_table.py for how the column is filled.
            auto brit = m_bodyByName.find(lowerName);
            const BodyKind kind = (brit != m_bodyByName.end()) ? brit->second : BodyKind::Generic;
            // Kliff-centric "PlayerSafe": an item is player-safe unless it is restricted to the female body.
            const bool isPlayer = (kind != BodyKind::Female);

            auto dit = m_displayNames.find(lowerName);
            std::string dispName = (dit != m_displayNames.end()) ? dit->second : std::string();

            // Canonical item-type code at desc+0x44 is authoritative. Unmapped codes (0x20 shield, 0x53 horse armor,
            // 0xFFFF quest, etc.) map to Count -> item is hidden as non-equipment. An absent type-code entry also
            // collapses to
            // Count; there's no name-parsing fallback because the engine itself reads this field to categorize the
            // item.
            TransmogSlot slot = TransmogSlot::Count;
            auto tcit = m_typeCode.find(id);
            if (tcit != m_typeCode.end())
                slot = slot_from_type_code(tcit->second);

            m_sortedCache.push_back({
                id,
                slot,
                hasVariant,
                isPlayer,
                equipT,
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
        // Lowercase into a stack buffer to avoid heap allocation. Item names in the catalog are bounded by k_maxNameLen
        // (256).
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
