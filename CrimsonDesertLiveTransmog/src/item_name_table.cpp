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

    // Item-type code, u16. This is the engine's own equip-slot key: SlotPopulator reads it out of the descriptor to
    // index its EquipTypeInfo table.
    //
    // LT does NOT interpret the VALUE. The value is a row index and renumbers whenever that table gains an entry,
    // which is exactly what made the old static switch rot. It is used only as a JOIN KEY: items the group taxonomy
    // already classified vote for their type code, and the winning slot then classifies every other item sharing that
    // code (see `learn` in build()).
    //
    // That join is what recovers NPC and boss gear. The engine files it under ItemGroup_Equip_Armor_Mon, which names
    // a family but no slot, so groups alone leave it unclassified -- yet it shares its type code with the player armor
    // of the same slot. It also keeps pet, horse, WarRobot and dragon gear OUT with no exclusion list: no player item
    // shares their codes, so nothing ever votes for them.
    //
    // The offset moves when the descriptor head changes width. A wrong value no longer mis-slots items silently: it
    // yields scattered keys that nothing votes for twice, so the learned table collapses and the [catalog-slots]
    // histogram drops to the group-only counts.
    static constexpr std::ptrdiff_t k_descTypeCodeOffset = 0x42;
    static constexpr uint16_t k_typeCodeNone = 0xFFFF; // arrows, quest items, anything with no equip slot

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
    // Driven by the item's own group membership, resolved by NAME at runtime. Every item descriptor carries
    // `_itemGroupInfoList`, a vector of u16 values naming the ItemGroupInfo rows the item belongs to. Those rows carry
    // the engine's own equipment taxonomy:
    //
    //     ..._Equip_Armor_Player_Helm / _Armor / _Cloak / _Gloves / _Boots
    //     ..._equip_accessory_Necklace / _Ring
    //     ..._Equip_Accessory_Glasses / _Mask
    //     ..._Equip_BackPack
    //     ..._Equip_Weapon_OneHand / _Shield / _TwoHand / _Range / _OneHandDagger
    //     ..._Equip_Tool / _Tool_NPC
    //
    // The taxonomy is stated twice, once as `ItemGroup_SubCategory_<tail>` and once as `ItemGroup_<tail>`, so the
    // rules match on the tail and cover both. Every item has at most ONE sub-category (verified catalog-wide: 5824
    // with one, 749 with none, none with two), and the family rows agree with it, so the match is unambiguous.
    //
    // Keying on the NAME is the point. The previous classifier read a u16 type code out of the descriptor and mapped
    // it through a static switch, but that code is a ROW INDEX into an engine table: inserting one row renumbers
    // everything above it, and the accessory band moved in both directions across past patches. A stale index table
    // does not fail loudly -- the codes the band vacates belong to WarRobot parts, so mech parts appear in the Glasses
    // and Mask pickers while real masks fall through to unmapped. Group names do not renumber.
    //
    // Non-player families (pet, horse, riding, vehicle) carry their own sub-categories and simply do not appear in the
    // table, so they classify as Count and stay out of every picker without needing an exclusion list.
    //
    // Three slots are absent from the taxonomy and are recovered from a more specific group the item also belongs to.
    // The engine spreads these across per-promotion rows (Equip_Tool_Lantern, Equip_Twitch_Special_Lantern, ...) with
    // no single covering row, so they match a short suffix instead:
    //
    //     *_Earring   -- earrings have no taxonomy row at all
    //     *_Lantern   -- lanterns are filed under Equip_Tool
    //     *_Band      -- bracelets sub-categorize as Control
    //
    // These outrank the taxonomy rules, which is what keeps lanterns out of the Tool picker.
    //
    // Name-parsing of the ITEM name is still not used: it produces false positives on anything whose name contains
    // tokens like "_Armor_" (horse armor, shields, quest treasure maps). Only group names are parsed, and those are
    // the engine's own classification rather than a display string.

    // Priority band for a group -> slot rule. Lower wins, so a specific-slot match beats the broad taxonomy: a lantern
    // is in a `*_Lantern` group AND in `Equip_Tool`, and it belongs in the Lantern picker.
    enum : std::uint8_t
    {
        k_groupPrioritySpecific = 0,
        k_groupPriorityTaxonomy = 1,
        k_groupPriorityNone = 0xFF,
    };

    struct GroupSlot
    {
        TransmogSlot slot = TransmogSlot::Count;
        std::uint8_t priority = k_groupPriorityNone;
    };

    struct GroupRule
    {
        std::string_view name;
        TransmogSlot slot;
    };

    // The taxonomy, matched on the END of the group name.
    //
    // Suffix rather than whole-name, because the engine states the same taxonomy twice: once as the item's
    // sub-category (`ItemGroup_SubCategory_Equip_Weapon_OneHand`) and once as a plain family row
    // (`ItemGroup_Equip_Weapon_OneHand`). They differ only by the `_SubCategory` infix, so one tail matches both. That
    // second layer is not redundant -- NPC props carry only the family row, which is how the boss knuckles reach
    // MainHand and the NPC torch, saw, drum, stick, priest wand and crutch reach Tool.
    //
    // Casing in the game data is inconsistent ("equip_accessory_Ring" vs "Equip_Accessory_Mask"), so every comparison
    // is case-insensitive. Tails are specific enough not to over-match: `_Equip_BackPack` does not catch
    // `Equip_SpecialBackPack` or `Equip_BackPack_Normal`, and `_Equip_Weapon_OneHand` does not catch
    // `Equip_Weapon_OneHandDagger`.
    static constexpr GroupRule k_taxonomyTailRules[] = {
        {"_Equip_Armor_Player_Helm", TransmogSlot::Helm},
        {"_Equip_Armor_Player_Armor", TransmogSlot::Chest},
        {"_Equip_Armor_Player_Cloak", TransmogSlot::Cloak},
        {"_Equip_Armor_Player_Gloves", TransmogSlot::Gloves},
        {"_Equip_Armor_Player_Boots", TransmogSlot::Boots},
        {"_Equip_Accessory_Necklace", TransmogSlot::Necklace},
        {"_Equip_Accessory_Ring", TransmogSlot::Ring1},
        {"_Equip_Accessory_Glasses", TransmogSlot::Glasses},
        {"_Equip_Accessory_Mask", TransmogSlot::Mask},
        {"_Equip_BackPack", TransmogSlot::Backpack},
        {"_Equip_Weapon_OneHand", TransmogSlot::MainHand},
        {"_Equip_Weapon_Shield", TransmogSlot::OffHand},
        {"_Equip_Weapon_TwoHand", TransmogSlot::TwoHandWeapon},
        {"_Equip_Weapon_Range", TransmogSlot::Ranged},
        {"_Equip_Weapon_OneHandDagger", TransmogSlot::SubWeapon},
        {"_Equip_Tool", TransmogSlot::Tool},
        {"_Equip_Tool_NPC", TransmogSlot::Tool},
    };

    // Suffix matches for the three slots the sub-category layer does not separate. Paired slots resolve to the
    // lower-indexed half; the picker shares its list across the pair via `slots_share_picker`, and the half actually
    // written is whichever row the user committed against.
    static constexpr GroupRule k_groupSuffixRules[] = {
        {"_Earring", TransmogSlot::Earring1},
        {"_Lantern", TransmogSlot::Lantern},
        {"_Band", TransmogSlot::Bracelet},
    };

    static bool iequals(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const auto ca = std::tolower(static_cast<unsigned char>(a[i]));
            const auto cb = std::tolower(static_cast<unsigned char>(b[i]));
            if (ca != cb)
                return false;
        }
        return true;
    }

    static bool iends_with(std::string_view name, std::string_view tail) noexcept
    {
        return name.size() >= tail.size() && iequals(name.substr(name.size() - tail.size()), tail);
    }

    static GroupSlot slot_from_group_name(std::string_view name) noexcept
    {
        for (const auto &rule : k_groupSuffixRules)
        {
            if (iends_with(name, rule.name))
                return {rule.slot, k_groupPrioritySpecific};
        }
        for (const auto &rule : k_taxonomyTailRules)
        {
            if (iends_with(name, rule.name))
                return {rule.slot, k_groupPriorityTaxonomy};
        }
        return {};
    }

    // --- ItemGroupInfo registry ---
    //
    // `_itemGroupInfoList` on the item descriptor: a vector of u16 group values. The generated deserializer resolves
    // each serialized key through the group registry's hash map at load time and stores `index + 1`, reserving 0 for
    // "key not found" (every item carries one such 0). So the def-array index is `value - 1`.
    // The vector is {qword data, dword size, dword capacity}. Size and capacity hold the same value on a loaded
    // descriptor, so reading the pair as one qword yields a huge number rather than a wrong-but-plausible count.
    static constexpr std::ptrdiff_t k_descItemGroupDataOffset = 0x350;  // qword, base of the u16 array
    static constexpr std::ptrdiff_t k_descItemGroupCountOffset = 0x358; // dword element count
    static constexpr std::size_t k_maxItemGroupsPerItem = 64;

    // The registry is a pa::StaticInfoManager2 like iteminfo -- same count and def-array offsets -- and its holder
    // sits in the same block of globals. The holder is FOUND rather than hardcoded: probe the neighbouring qwords and
    // keep the first whose rows carry "ItemGroup..." names. Exactly one candidate in the window qualifies, so the
    // probe self-heals when a patch reorders that block.
    static constexpr std::ptrdiff_t k_groupHolderProbeLow = -0x80;
    static constexpr std::ptrdiff_t k_groupHolderProbeHigh = 0x100;
    static constexpr std::size_t k_groupHolderProbeRows = 16; // rows sampled per candidate before rejecting it

    static constexpr std::ptrdiff_t k_groupRowDefOffset = 0x18; // registry row -> group-def object

    // The name's {ptr,len} wrapper sits at a VARIABLE offset inside the def object: a name short enough to live in the
    // object's inline buffer pushes the wrapper past it, and the neighbouring member is a variable-length u16 array of
    // member item ids. Scan a bounded window for a pair that resolves to a string of exactly the stated length whose
    // prefix is "ItemGroup". A wrong pair fails all three checks, so the scan cannot silently pick up a neighbour.
    static constexpr std::ptrdiff_t k_groupNameScanBegin = 0x18;
    static constexpr std::ptrdiff_t k_groupNameScanEnd = 0x60;
    static constexpr std::size_t k_maxGroupNameLen = 160;
    static constexpr std::string_view k_groupNamePrefix = "ItemGroup";

    static constexpr uint32_t k_maxGroupCount = 0x20000;

    /**
     * Read one group row's name. Returns an empty string when the row does not resolve to an "ItemGroup..." name.
     */
    static std::string read_group_name(uintptr_t row) noexcept
    {
        bool ok = false;
        const uintptr_t def = read_qword_safe(row + k_groupRowDefOffset, ok);
        if (!ok || !DMKMemory::plausible_userspace_ptr(def))
            return {};

        char buf[k_maxGroupNameLen + 1];
        for (std::ptrdiff_t off = k_groupNameScanBegin; off <= k_groupNameScanEnd; off += 4)
        {
            const uintptr_t strPtr = read_qword_safe(def + off, ok);
            if (!ok || !DMKMemory::plausible_userspace_ptr(strPtr))
                continue;
            const uint32_t len = read_u32_safe(def + off + 8, ok);
            if (!ok || len < k_groupNamePrefix.size() || len > k_maxGroupNameLen)
                continue;
            const auto got = read_cstring_safe(strPtr, buf, sizeof(buf));
            if (got != len)
                continue;
            const std::string_view name(buf, got);
            if (name.substr(0, k_groupNamePrefix.size()) == k_groupNamePrefix)
                return std::string(name);
        }
        return {};
    }

    /**
     * Locate the ItemGroupInfo registry holder by probing the globals around the iteminfo holder. Returns 0 when no
     * candidate in the window exposes "ItemGroup..." rows. Cached by the caller, because the answer is an address of a
     * global and does not change within a process lifetime.
     */
    static uintptr_t probe_group_registry_holder(uintptr_t iteminfoHolder) noexcept
    {
        for (std::ptrdiff_t off = k_groupHolderProbeLow; off <= k_groupHolderProbeHigh; off += 8)
        {
            const uintptr_t holder = iteminfoHolder + off;
            bool ok = false;
            const uintptr_t mgr = read_qword_safe(holder, ok);
            if (!ok || !DMKMemory::plausible_userspace_ptr(mgr))
                continue;

            const uint32_t count = read_u32_safe(mgr + k_iteminfoCountOffset, ok);
            if (!ok || count == 0 || count > k_maxGroupCount)
                continue;
            const uintptr_t rows = read_qword_safe(mgr + k_iteminfoPtrArrayOffset, ok);
            if (!ok || !DMKMemory::plausible_userspace_ptr(rows))
                continue;

            const auto sample = (std::min)(static_cast<std::size_t>(count), k_groupHolderProbeRows);
            for (std::size_t i = 0; i < sample; ++i)
            {
                const uintptr_t row = read_qword_safe(rows + i * 8ull, ok);
                if (!ok || !DMKMemory::plausible_userspace_ptr(row))
                    continue;
                if (!read_group_name(row).empty())
                    return holder;
            }
        }
        return 0;
    }

    /**
     * Build `defIndex -> GroupSlot` for the whole registry. An empty result means the registry did not resolve, which
     * the caller treats as "defer and retry" rather than publishing an unclassified catalog.
     */
    static std::vector<GroupSlot> build_group_slot_table(uintptr_t groupHolder, std::size_t &mappedOut) noexcept
    {
        mappedOut = 0;
        std::vector<GroupSlot> table;

        bool ok = false;
        const uintptr_t mgr = read_qword_safe(groupHolder, ok);
        if (!ok || !DMKMemory::plausible_userspace_ptr(mgr))
            return table;
        const uint32_t count = read_u32_safe(mgr + k_iteminfoCountOffset, ok);
        if (!ok || count == 0 || count > k_maxGroupCount)
            return table;
        const uintptr_t rows = read_qword_safe(mgr + k_iteminfoPtrArrayOffset, ok);
        if (!ok || !DMKMemory::plausible_userspace_ptr(rows))
            return table;

        table.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uintptr_t row = read_qword_safe(rows + i * 8ull, ok);
            if (!ok || !DMKMemory::plausible_userspace_ptr(row))
                continue;
            const auto name = read_group_name(row);
            if (name.empty())
                continue;
            const auto mapped = slot_from_group_name(name);
            if (mapped.priority == k_groupPriorityNone)
                continue;
            table[i] = mapped;
            ++mappedOut;
        }
        return table;
    }

    /**
     * Classify one item from its group membership. Returns Count when the item belongs to no mapped group, which is
     * the normal answer for consumables, quest items and non-player equipment.
     */
    static TransmogSlot slot_from_item_groups(uintptr_t descPtr, const std::vector<GroupSlot> &groupSlots) noexcept
    {
        bool ok = false;
        const uintptr_t data = read_qword_safe(descPtr + k_descItemGroupDataOffset, ok);
        if (!ok || !DMKMemory::plausible_userspace_ptr(data))
            return TransmogSlot::Count;
        const uint32_t count = read_u32_safe(descPtr + k_descItemGroupCountOffset, ok);
        if (!ok || count == 0 || count > k_maxItemGroupsPerItem)
            return TransmogSlot::Count;

        GroupSlot best;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint16_t value = read_u16_safe(data + i * 2ull, ok);
            if (!ok || value == 0) // 0 is the deserializer's "key not resolved" filler
                continue;
            const std::size_t index = value - 1u;
            if (index >= groupSlots.size())
                continue;
            const auto &candidate = groupSlots[index];
            if (candidate.priority < best.priority)
                best = candidate;
        }
        return best.slot;
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

        // Step C: resolve the ItemGroupInfo registry and turn it into `defIndex -> slot`. Slot classification depends
        // on it entirely, so a miss defers rather than publishing a catalog in which every item reads as
        // non-equipment. The holder address is stable for the process, so it is probed once.
        static uintptr_t s_groupHolder = 0;
        if (s_groupHolder == 0)
        {
            s_groupHolder = probe_group_registry_holder(globalHolder);
            if (s_groupHolder == 0)
            {
                logger.trace("[nametable] ItemGroupInfo registry not found near the iteminfo holder "
                             "(0x{:X}) -- deferring",
                             globalHolder);
                return BuildResult::Deferred;
            }
            logger.info("[nametable] ItemGroupInfo holder = 0x{:X} (iteminfo holder {:+#x})",
                        s_groupHolder, static_cast<std::ptrdiff_t>(s_groupHolder - globalHolder));
        }

        std::size_t mappedGroups = 0;
        const auto groupSlots = build_group_slot_table(s_groupHolder, mappedGroups);
        if (groupSlots.empty() || mappedGroups == 0)
        {
            logger.trace("[nametable] ItemGroupInfo registry empty or unnamed "
                         "({} rows, {} mapped) -- deferring",
                         groupSlots.size(), mappedGroups);
            return BuildResult::Deferred;
        }

        logger.info("[nametable] scanning item catalog: count={} "
                    "globalPtr=0x{:X} ptrArray=0x{:X} "
                    "(item groups: {} rows, {} mapped to slots)",
                    count, globalPtr, ptrArray, groupSlots.size(), mappedGroups);

        const auto t0 = std::chrono::steady_clock::now();

        // Build into local maps first so the published snapshot is atomic from any reader's viewpoint. Only copy into
        // the member maps under the mutex once walking is done.
        std::unordered_map<uint16_t, std::string> idToName;
        std::unordered_map<std::string, uint16_t> nameToId;
        std::unordered_map<uint16_t, uint8_t> variantFlag;
        std::unordered_map<uint16_t, TransmogSlot> slotMap;
        idToName.reserve(count);
        nameToId.reserve(count);
        variantFlag.reserve(count);
        slotMap.reserve(count);

        // Pass 1: walk the catalog and collect the name, the variant-meta pointer and the transmog slot for every
        // valid descriptor. The variant flag cannot be resolved yet, because pass 2 derives the sentinel
        // statistically from the values this pass collects.
        struct ScratchEntry
        {
            uint16_t id;
            std::string name;
            uintptr_t metaPtr;  // 0 on read fault
            TransmogSlot slot;  // from the item's group membership, Count when its groups name no slot
            uint16_t typeCode;  // join key for the learned pass below
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
            // Transmog slot from the item's own group membership. See the slot-classification block at the top of this
            // file: the item names the ItemGroupInfo rows it belongs to, and those rows carry the engine's equipment
            // taxonomy by NAME. Anything not in a mapped group (consumables, quest items, pet and mount gear) resolves
            // to Count and stays out of every picker.
            // Items whose groups name no slot (NPC and boss gear, which files under Armor_Mon) stay Count here and are
            // resolved by the learned type-code pass after the walk.
            const TransmogSlot slot = slot_from_item_groups(descPtr, groupSlots);

            bool tcOk = false;
            const uint16_t typeCode = read_u16_safe(descPtr + k_descTypeCodeOffset, tcOk);

            scratch.push_back({
                id16,
                std::string(buf, len),
                ok ? metaPtr : 0,
                slot,
                tcOk ? typeCode : k_typeCodeNone,
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

        // Pass 2b -- learn `typeCode -> slot` from the items the group taxonomy classified.
        //
        // The group names carry the slot only for PLAYER equipment. NPC and boss gear sits in families like
        // ItemGroup_Equip_Armor_Mon that name no slot, so groups alone leave roughly two thirds of the wearable
        // catalog unclassified. The type code closes that gap: a boss helm shares its code with player helms.
        //
        // Majority vote rather than first-wins, because a handful of items carry a sub-category that disagrees with
        // their code (one item votes Helm for the chest code). Those lose by two orders of magnitude. A contested code
        // is logged, not suppressed -- a code that starts splitting evenly means the join stopped being sound.
        std::unordered_map<uint16_t, TransmogSlot> learnedSlot;
        std::size_t contestedCodes = 0;
        {
            std::unordered_map<uint16_t, std::unordered_map<TransmogSlot, uint32_t>> votes;
            for (const auto &e : scratch)
            {
                if (e.slot != TransmogSlot::Count && e.typeCode != k_typeCodeNone)
                    ++votes[e.typeCode][e.slot];
            }

            learnedSlot.reserve(votes.size());
            for (const auto &[code, tally] : votes)
            {
                TransmogSlot winner = TransmogSlot::Count;
                uint32_t topVotes = 0;
                uint32_t total = 0;
                for (const auto &[slot, n] : tally)
                {
                    total += n;
                    if (n > topVotes)
                    {
                        topVotes = n;
                        winner = slot;
                    }
                }
                if (topVotes < total)
                {
                    ++contestedCodes;
                    logger.trace("[catalog-slots] type code {:#06x} contested: {} of {} votes for {}",
                                 code, topVotes, total, slot_name(winner));
                }
                learnedSlot.emplace(code, winner);
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

            TransmogSlot slot = e.slot;
            if (slot == TransmogSlot::Count && e.typeCode != k_typeCodeNone)
            {
                if (auto lit = learnedSlot.find(e.typeCode); lit != learnedSlot.end())
                    slot = lit->second;
            }
            if (slot != TransmogSlot::Count)
                slotMap.emplace(e.id, slot);
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

        // Per-slot histogram of the classification, with sample names. A patch that reshapes the group registry or
        // renames a sub-category shows up here as a slot that went to zero, which is the failure this classifier is
        // meant to make visible: the old type-code table failed silently by listing the WRONG items instead.
        {
            std::unordered_map<TransmogSlot, std::vector<std::uint16_t>> bucket;
            bucket.reserve(static_cast<std::size_t>(TransmogSlot::Count));
            for (const auto &kv : slotMap)
                bucket[kv.second].push_back(kv.first);

            logger.trace("[catalog-slots] {}/{} items classified across {} slots "
                         "({} type codes learned from group names, {} contested)",
                         slotMap.size(), valid, bucket.size(), learnedSlot.size(), contestedCodes);

            for (std::uint8_t s = 0; s < static_cast<std::uint8_t>(TransmogSlot::Count); ++s)
            {
                const auto slot = static_cast<TransmogSlot>(s);
                auto it = bucket.find(slot);
                if (it == bucket.end())
                    continue;

                // Sort itemIds ascending and pick the first 3 names for a stable sample window.
                auto &ids = it->second;
                std::sort(ids.begin(), ids.end());
                const auto take = std::min<std::size_t>(3, ids.size());

                std::string samples;
                for (std::size_t k = 0; k < take; ++k)
                {
                    auto nit = idToName.find(ids[k]);
                    if (k > 0)
                        samples += ", ";
                    samples += (nit != idToName.end()) ? nit->second : "<unknown>";
                }

                logger.trace("[catalog-slots]   {:<13} count={:>4} samples: {}", slot_name(slot), ids.size(), samples);
            }
        }

        {
            std::lock_guard<std::mutex> lk(s_tableMtx);
            m_idToName = std::move(idToName);
            m_nameToId = std::move(nameToId);
            m_variantFlag = std::move(variantFlag);
            m_slotById = std::move(slotMap);
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
        // Runtime-observed binding wins. If the engine actually equipped this itemId in a slot, that is ground truth
        // and beats the catalog classification.
        if (auto obs = m_observedSlot.find(itemId); obs != m_observedSlot.end())
            return obs->second;

        auto it = m_slotById.find(itemId);
        return (it != m_slotById.end()) ? it->second : TransmogSlot::Count;
    }

    TransmogSlot ItemNameTable::catalog_category_of(uint16_t itemId) const noexcept
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        auto it = m_slotById.find(itemId);
        return (it != m_slotById.end()) ? it->second : TransmogSlot::Count;
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

            // The item's group membership is authoritative. Anything with no mapped group (pet and mount gear, quest
            // items, consumables) is absent from the map and collapses to Count, hiding it as non-equipment. There is
            // no name-parsing fallback: the groups ARE the engine's classification.
            auto slit = m_slotById.find(id);
            const TransmogSlot slot = (slit != m_slotById.end()) ? slit->second : TransmogSlot::Count;

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
