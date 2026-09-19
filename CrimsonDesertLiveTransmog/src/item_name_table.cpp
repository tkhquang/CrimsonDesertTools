#include "item_name_table.hpp"
#include "aob_resolver.hpp"
#include "language_pack.hpp"
#include "shared_state.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Transmog
{
    // Sensible upper bound on item descriptor catalog size. The game ships several thousand entries. Guard generously.
    static constexpr uint32_t MAX_CATALOG_SIZE = 0x20000;

    static constexpr std::size_t MAX_NAME_LEN = 96;

    /**
     * @brief ASCII-only case fold for one byte.
     * @details `[B-37]` requires a locale-independent fold on a resolution path. Catalog internal names carry
     *          non-ASCII bytes, notably the Unicode Roman numerals, and `std::tolower` is locale dependent above 0x7F.
     *          A CRT locale change would fold those bytes differently in two layers and split one item across two
     *          keys, which reads as a missing display name rather than as a fold bug.
     */
    static constexpr char ascii_lower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    /// Builds the lowercased internal-name key that every display-name and wearer-body map uses.
    [[nodiscard]] static std::string fold_key(std::string_view name)
    {
        std::string key{name};
        for (auto &c : key)
            c = ascii_lower(c);
        return key;
    }

    // Variant-metadata detection (see item_name_table.hpp::has_variant_meta). Clean base items have `*(desc+<offset>)
    // == <sentinel>`, where <sentinel> is a shared empty-object pointer - an IRefCounted vtable in the exe's .data
    // section. Non-sentinel values point to a per-item metadata struct threaded through a catalog-wide linked list.
    // Members of that list do not render via runtime transmog.
    //
    // The field offset moves whenever the descriptor struct changes shape. The region grows on some patches and
    // SHRINKS on others, so do not extrapolate a direction from the last move. A stale offset fails SILENTLY: the
    // neighboring qwords hold plausible values too (a constant 0, or an unrelated bit pattern such as 0x1FFFFFFFF), so
    // nothing trips and every item classifies wrong.
    //
    // Re-derive the offset like this. probe every descriptor qword across a 0x380..0x480 window, then correlate
    // "value != mode" for each candidate offset against the Variant column of the previous game version's item dump,
    // matched BY NAME. The correct offset produces no false positives. No other offset in the window comes close, so
    // the result is unambiguous. At least one other descriptor offset also holds a sentinel on every item, so "most
    // items share this value" alone does NOT identify the field. The deciding constraint is: sentinel for direct-wear
    // items, per-item heap pointer for carrier-required items.
    //
    // The sentinel value itself is also unstable, because any .data reshuffle moves it. The builder hardcodes neither
    // the offset nor the sentinel address. It resolves the sentinel statistically at catalog-build time. It scans
    // every valid descriptor's qword at this offset, and the value that appears in the clear majority of items IS the
    // sentinel. This self-heals across future game updates as long as base items keep their statistical dominance of
    // the catalog.
    static constexpr std::ptrdiff_t DESC_VARIANT_META_OFFSET = 0x3B0;

    // Item-type code, u16. This is the engine's own equip-slot key: SlotPopulator reads it out of the descriptor to
    // index its EquipTypeInfo table.
    //
    // LT does NOT interpret the VALUE. The value is a row index and renumbers whenever that table gains an entry, so
    // it is a JOIN KEY and never a slot key. Items the group taxonomy already classified vote for their type code,
    // and the winning slot then classifies every other item that shares that code (see the learned pass in build()).
    //
    // That join recovers NPC and boss gear. The engine files it under ItemGroup_Equip_Armor_Mon, which names a family
    // but no slot, so groups alone leave it unclassified - yet it shares its type code with the player armor of the
    // same slot. It also keeps pet, horse, WarRobot and dragon gear OUT with no exclusion list: no player item shares
    // their codes, so nothing ever votes for them.
    //
    // The offset moves when the descriptor head changes width. A wrong value no longer mis-slots items silently: it
    // yields scattered keys that nothing votes for twice, so the learned table collapses and the [catalog-slots]
    // histogram drops to the group-only counts.
    static constexpr std::ptrdiff_t DESC_TYPE_CODE_OFFSET = 0x42;
    /// Arrows, quest items, anything with no equip slot.
    static constexpr uint16_t TYPE_CODE_NONE = 0xFFFF;

    // iteminfo container layout
    // These are runtime data offsets, not code, so they cannot be AOB-scanned. If a future patch reshapes the struct,
    // the catalog walk produces an implausible count or ptr_array and bails at the sanity checks below. Read the live
    // values off the `mov rax,[rbx+<offset>]` that ItemAccessor uses to reach the array.
    //
    // WARNING - the sanity checks do NOT catch a small shift of the array offset. The offset moves when the
    // pa::StaticInfoManager2 base that iteminfo derives from changes width. A shifted offset still dereferences to a
    // valid-looking heap pointer, so the is_plausible_ptr guard stays silent and the walk emits garbage item names
    // instead of failing. The StringInfo registry rides the same base and moves with it (see prefab_wrapper_swap.cpp),
    // but a change to that base does NOT move every member by the same amount, and it does not always move them in the
    // same direction. The count offset and the array offset move independently. Verify each one against live memory on
    // patch day. Do not trust the guards.
    /// Dword entry count.
    static constexpr std::ptrdiff_t ITEMINFO_COUNT_OFFSET = 0x08;
    /// Qword base of the descriptor pointer array.
    static constexpr std::ptrdiff_t ITEMINFO_PTR_ARRAY_OFFSET = 0x58;

    // Safe memory helpers
    //
    // The `(value, bool& ok)` shape distinguishes a faulted read from a legitimate zero result, which matters at call
    // sites where 0 is a valid value (e.g. slot index 0 versus unread slot field). `memory::read<T>` is the
    // underlying SEH-protected primitive. These adapters fold its `Result<T>` return into the local shape used by the
    // rest of this translation unit.

    static uintptr_t read_qword_safe(uintptr_t addr, bool &ok) noexcept
    {
        const auto v = DMK::memory::read<uintptr_t>(DMK::Address{addr});
        ok = v.has_value();
        return v.value_or(0);
    }

    static uint32_t read_u32_safe(uintptr_t addr, bool &ok) noexcept
    {
        const auto v = DMK::memory::read<uint32_t>(DMK::Address{addr});
        ok = v.has_value();
        return v.value_or(0);
    }

    static uint16_t read_u16_safe(uintptr_t addr, bool &ok) noexcept
    {
        const auto v = DMK::memory::read<uint16_t>(DMK::Address{addr});
        ok = v.has_value();
        return v.value_or(0);
    }

    /// Read a null-terminated ASCII string from `str_ptr` into `buf`.
    static std::size_t read_cstring_safe(uintptr_t str_ptr, char *buf, std::size_t buf_size) noexcept
    {
        __try
        {
            const auto *src = reinterpret_cast<const char *>(str_ptr);
            std::size_t len = 0;
            while (len < buf_size - 1 && src[len] != '\0')
            {
                const auto c = src[len];
                // Reject control bytes (0x01..0x1F) - they signal a misaligned heap read. Accept 0x80..0xFF: some
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

    // Slot classification
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
    // The engine states the taxonomy twice, once as `ItemGroup_SubCategory_<tail>` and once as `ItemGroup_<tail>`, so
    // the rules match on the tail and cover both. Every item has at most ONE sub-category, but the two statements do
    // NOT always agree - a knuckledrill sub-categorizes as Tool while its family row is Weapon_OneHand. Where they
    // disagree the family row wins, through the priority band below.
    //
    // The NAME is the key, because a type code is a ROW INDEX into an engine table. One inserted row renumbers
    // everything above it, and the accessory band moved in both directions across past patches. A stale index table
    // does not fail loudly: the codes the band vacates belong to WarRobot parts, so mech parts appear in the Glasses
    // and Mask pickers while real masks fall through to unmapped. Group names do not renumber.
    //
    // Non-player families (pet, horse, riding, vehicle) carry their own sub-categories and do not appear in the table,
    // so they classify as Count and stay out of every picker with no exclusion list.
    //
    // Classification runs in three priority bands, and the lowest number wins: a short slot-name tail (Specific), a
    // taxonomy family tail (Taxonomy), and either of those matched on an `ItemGroup_SubCategory_*` row
    // (SubCategory). The matcher strips a tiered qualifier such as `_Tier3` first, because it sits exactly where a
    // tail expects the group name to end.
    //
    // Several slots have no single covering taxonomy row. The classifier recovers each one from a more specific group
    // the item also belongs to. The engine spreads them across per-promotion rows (Equip_Tool_Lantern,
    // Equip_Twitch_Special_Lantern, Accessory_Special_Necklace_Tier4, ...), so they match a short suffix instead:
    //
    //     *_Earring    - earrings have no taxonomy row at all
    //     *_Necklace   - also reached through an Earring sub-category, which the priority band demotes
    //     *_Ring       - same, and it cannot collide with *_Earring (see GROUP_SUFFIX_RULES)
    //     *_Glasses    - tiered and Special variants carry an extra path component
    //     *_Mask       - same
    //     *_Lantern    - lanterns are filed under Equip_Tool
    //     *_Band       - bracelets sub-categorize as Control
    //
    // These outrank the taxonomy rules, which is what keeps lanterns out of the Tool picker.
    //
    // The classifier never parses the ITEM name: that produces false positives on anything whose name contains
    // tokens like "_Armor_" (horse armor, shields, quest treasure maps). It parses only group names, which carry the
    // engine's own classification rather than a display string.

    // Priority band for a group -> slot rule. Lower wins, so a specific-slot match beats the broad taxonomy: a lantern
    // is in a `*_Lantern` group AND in `Equip_Tool`, and it belongs in the Lantern picker.
    enum : std::uint8_t
    {
        GROUP_PRIORITY_SPECIFIC = 0,
        GROUP_PRIORITY_TAXONOMY = 1,
        // An `ItemGroup_SubCategory_*` row is the engine's COARSE bucket for an item, and it does not always agree
        // with the item's own family row: a necklace sits in `ItemGroup_SubCategory_Equip_accessory_Earring` while
        // also sitting in `ItemGroup_equip_accessory_Necklace`, and a knuckledrill sits in
        // `ItemGroup_SubCategory_Equip_Tool` while also sitting in `ItemGroup_Equip_Weapon_OneHand`. Scoring both
        // rows in the same band leaves the winner to whichever the walk reaches first, which is not a decision.
        //
        // This band applies to BOTH rule tables and to every axis, not only accessories. Wherever a sub-category row
        // and a family row name different slots, the FAMILY row wins: a necklace stops resolving to Earring1, a
        // knuckledrill files under MainHand rather than Tool, and a robe whose sub-category says Helm files under
        // Chest. An item carrying nothing but a sub-category row still classifies, one band lower.
        GROUP_PRIORITY_SUB_CATEGORY = 2,
        GROUP_PRIORITY_NONE = 0xFF,
    };

    struct GroupSlot
    {
        TransmogSlot slot = TransmogSlot::Count;
        std::uint8_t priority = GROUP_PRIORITY_NONE;
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
    // second layer is not redundant - NPC props carry only the family row, which is how the boss knuckles reach
    // MainHand and the NPC torch, saw, drum, stick, priest wand and crutch reach Tool.
    //
    // Casing in the game data is inconsistent ("equip_accessory_Ring" vs "Equip_Accessory_Mask"), so every comparison
    // is case-insensitive. Tails are specific enough not to over-match: `_Equip_BackPack` does not catch
    // `Equip_SpecialBackPack` or `Equip_BackPack_Normal`, and `_Equip_Weapon_OneHand` does not catch
    // `Equip_Weapon_OneHandDagger`.
    static constexpr GroupRule TAXONOMY_TAIL_RULES[] = {
        {"_Equip_Armor_Player_Helm", TransmogSlot::Helm},
        {"_Equip_Armor_Player_Armor", TransmogSlot::Chest},
        {"_Equip_Armor_Player_Cloak", TransmogSlot::Cloak},
        {"_Equip_Armor_Player_Gloves", TransmogSlot::Gloves},
        {"_Equip_Armor_Player_Boots", TransmogSlot::Boots},
        {"_Equip_BackPack", TransmogSlot::Backpack},
        {"_Equip_Weapon_OneHand", TransmogSlot::MainHand},
        {"_Equip_Weapon_Shield", TransmogSlot::OffHand},
        {"_Equip_Weapon_TwoHand", TransmogSlot::TwoHandWeapon},
        {"_Equip_Weapon_Range", TransmogSlot::Ranged},
        {"_Equip_Weapon_OneHandDagger", TransmogSlot::SubWeapon},
        {"_Equip_Tool", TransmogSlot::Tool},
        {"_Equip_Tool_NPC", TransmogSlot::Tool},
    };

    // Suffix matches for the slots the sub-category layer does not separate, scanned BEFORE the taxonomy tails and
    // scored one band higher. Paired slots resolve to the lower-indexed half. The picker shares its list across the
    // pair via `slots_share_picker`, and the half actually written is whichever row the user committed against.
    //
    // The accessory tails are deliberately just the slot name. The engine spreads one accessory slot across several
    // path shapes (`..._Equip_Accessory_Necklace`, `..._Accessory_Special_Necklace_Tier4`), and a tail long enough
    // to name the family misses every variant that carries an extra path component. Anything longer than the slot
    // name belongs in TAXONOMY_TAIL_RULES, and a tail that appears in both tables makes the taxonomy row dead -
    // the scan here returns first.
    //
    // `_Earring` precedes `_Ring` for clarity only. They cannot collide, because an earring group ends "arring" and
    // the ring tail requires the leading underscore.
    static constexpr GroupRule GROUP_SUFFIX_RULES[] = {
        // Accessory slots, matched after strip_tier_suffix drops the tier qualifier.
        {"_Earring", TransmogSlot::Earring1},
        {"_Necklace", TransmogSlot::Necklace},
        {"_Ring", TransmogSlot::Ring1},
        {"_Glasses", TransmogSlot::Glasses},
        {"_Mask", TransmogSlot::Mask},
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

    /// Case-insensitive substring test. Only used for the unmatched-group report, never for classification.
    static bool icontains(std::string_view hay, std::string_view needle) noexcept
    {
        if (needle.size() > hay.size())
            return false;
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
        {
            if (iequals(hay.substr(i, needle.size()), needle))
                return true;
        }
        return false;
    }

    /**
     * @brief Drop a trailing `_Tier<digits>` qualifier from a group name.
     *
     * @details Accessory groups are tiered - `ItemGroup_Equip_Accessory_Necklace_Tier3`,
     *          `..._Accessory_Special_Ring_Tier4`. Every rule matches with `iends_with`, so a tiered name matches
     *          NOTHING: the tier sits exactly where a rule expects the group to end. The failure is silent and total
     *          for the affected slot. Its bucket classifies zero items, and those items then inherit whatever slot
     *          the learned pass took from some other group that did match. That is how necklaces reach the earring
     *          bucket.
     *
     *          Strips one exact `_Tier` + digits tail and nothing else, so a group that genuinely ends in something
     *          else is untouched. Returns the name unchanged when there is no such tail.
     */
    static std::string_view strip_tier_suffix(std::string_view name) noexcept
    {
        auto end = name.size();
        while (end > 0 && name[end - 1] >= '0' && name[end - 1] <= '9')
            --end;
        if (end == name.size())
            return name; // no trailing digits - nothing to strip
        constexpr std::string_view tier = "_Tier";
        if (end < tier.size() || !iequals(name.substr(end - tier.size(), tier.size()), tier))
            return name;
        return name.substr(0, end - tier.size());
    }

    static GroupSlot slot_from_group_name(std::string_view name) noexcept
    {
        // Try the raw name first so a rule that deliberately ends in digits still wins on an exact match. The
        // tier-stripped form is the fallback.
        const auto base = strip_tier_suffix(name);
        const bool sub_category = icontains(name, "_SubCategory_");
        for (const auto &rule : GROUP_SUFFIX_RULES)
        {
            if (iends_with(name, rule.name) || iends_with(base, rule.name))
                return {rule.slot, sub_category ? GROUP_PRIORITY_SUB_CATEGORY : GROUP_PRIORITY_SPECIFIC};
        }
        for (const auto &rule : TAXONOMY_TAIL_RULES)
        {
            if (iends_with(name, rule.name) || iends_with(base, rule.name))
                return {rule.slot, sub_category ? GROUP_PRIORITY_SUB_CATEGORY : GROUP_PRIORITY_TAXONOMY};
        }
        return {};
    }

    // ItemGroupInfo registry
    //
    // `_itemGroupInfoList` on the item descriptor: a vector of u16 group values. The generated deserializer resolves
    // each serialized key through the group registry's hash map at load time and stores `index + 1`, reserving 0 for
    // "key not found" (every item carries one such 0). So the def-array index is `value - 1`.
    // The vector is {qword data, dword size, dword capacity}. Size and capacity hold the same value on a loaded
    // descriptor, so reading the pair as one qword yields a huge number rather than a wrong-but-plausible count.
    /// Qword, base of the u16 group-value array.
    static constexpr std::ptrdiff_t DESC_ITEM_GROUP_DATA_OFFSET = 0x350;
    /// Dword element count.
    static constexpr std::ptrdiff_t DESC_ITEM_GROUP_COUNT_OFFSET = 0x358;
    static constexpr std::size_t MAX_ITEM_GROUPS_PER_ITEM = 64;

    // The registry is a pa::StaticInfoManager2 like iteminfo - same count and def-array offsets - and its holder
    // sits in the same block of globals. The holder is FOUND rather than hardcoded: probe the neighboring qwords and
    // keep the first whose rows carry "ItemGroup..." names. Exactly one candidate in the window qualifies, so the
    // probe self-heals when a patch reorders that block.
    static constexpr std::ptrdiff_t GROUP_HOLDER_PROBE_LOW = -0x80;
    static constexpr std::ptrdiff_t GROUP_HOLDER_PROBE_HIGH = 0x100;
    /// Rows sampled per candidate before the probe rejects it.
    static constexpr std::size_t GROUP_HOLDER_PROBE_ROWS = 16;

    /// Registry row -> group-def object.
    static constexpr std::ptrdiff_t GROUP_ROW_DEF_OFFSET = 0x18;

    // The name's {ptr,len} wrapper sits at a VARIABLE offset inside the def object: a name short enough to live in the
    // object's inline buffer pushes the wrapper past it, and the neighboring member is a variable-length u16 array of
    // member item ids. Scan a bounded window for a pair that resolves to a string of exactly the stated length whose
    // prefix is "ItemGroup". A wrong pair fails all three checks, so the scan cannot silently pick up a neighbor.
    static constexpr std::ptrdiff_t GROUP_NAME_SCAN_BEGIN = 0x18;
    static constexpr std::ptrdiff_t GROUP_NAME_SCAN_END = 0x60;
    static constexpr std::size_t MAX_GROUP_NAME_LEN = 160;
    static constexpr std::string_view GROUP_NAME_PREFIX = "ItemGroup";

    static constexpr uint32_t MAX_GROUP_COUNT = 0x20000;

    /**
     * @brief Read one group row's name.
     * @return The name, or an empty string when the row does not resolve to an "ItemGroup..." name.
     */
    static std::string read_group_name(uintptr_t row) noexcept
    {
        bool ok = false;
        const uintptr_t def = read_qword_safe(row + GROUP_ROW_DEF_OFFSET, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{def}))
            return {};

        char buf[MAX_GROUP_NAME_LEN + 1];
        for (std::ptrdiff_t off = GROUP_NAME_SCAN_BEGIN; off <= GROUP_NAME_SCAN_END; off += 4)
        {
            const uintptr_t str_ptr = read_qword_safe(def + off, ok);
            if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{str_ptr}))
                continue;
            const uint32_t len = read_u32_safe(def + off + 8, ok);
            if (!ok || len < GROUP_NAME_PREFIX.size() || len > MAX_GROUP_NAME_LEN)
                continue;
            const auto got = read_cstring_safe(str_ptr, buf, sizeof(buf));
            if (got != len)
                continue;
            const std::string_view name(buf, got);
            if (name.substr(0, GROUP_NAME_PREFIX.size()) == GROUP_NAME_PREFIX)
                return std::string(name);
        }
        return {};
    }

    /**
     * @brief Locate the ItemGroupInfo registry holder in the globals around the iteminfo holder.
     * @return The holder address, or 0 when no candidate in the window exposes "ItemGroup..." rows.
     * @note The caller caches the answer: it is the address of a global and stays fixed for the process.
     */
    static uintptr_t probe_group_registry_holder(uintptr_t iteminfo_holder) noexcept
    {
        for (std::ptrdiff_t off = GROUP_HOLDER_PROBE_LOW; off <= GROUP_HOLDER_PROBE_HIGH; off += 8)
        {
            const uintptr_t holder = iteminfo_holder + off;
            bool ok = false;
            const uintptr_t mgr = read_qword_safe(holder, ok);
            if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{mgr}))
                continue;

            const uint32_t count = read_u32_safe(mgr + ITEMINFO_COUNT_OFFSET, ok);
            if (!ok || count == 0 || count > MAX_GROUP_COUNT)
                continue;
            const uintptr_t rows = read_qword_safe(mgr + ITEMINFO_PTR_ARRAY_OFFSET, ok);
            if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{rows}))
                continue;

            const auto sample = (std::min)(static_cast<std::size_t>(count), GROUP_HOLDER_PROBE_ROWS);
            for (std::size_t i = 0; i < sample; ++i)
            {
                const uintptr_t row = read_qword_safe(rows + i * 8ull, ok);
                if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{row}))
                    continue;
                if (!read_group_name(row).empty())
                    return holder;
            }
        }
        return 0;
    }

    /// Upper bound on the unmatched-accessory-group names named in one report line.
    static constexpr std::size_t UNMATCHED_GROUP_REPORT_CAP = 24;

    /**
     * @brief Build `defIndex -> GroupSlot` for the whole registry.
     * @return One entry per registry row. An empty result means the registry did not resolve, and the caller then
     *         defers and retries rather than publishing an unclassified catalog.
     */
    static std::vector<GroupSlot> build_group_slot_table(uintptr_t group_holder, std::size_t &mapped_out) noexcept
    {
        mapped_out = 0;
        std::vector<GroupSlot> table;

        bool ok = false;
        const uintptr_t mgr = read_qword_safe(group_holder, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{mgr}))
            return table;
        const uint32_t count = read_u32_safe(mgr + ITEMINFO_COUNT_OFFSET, ok);
        if (!ok || count == 0 || count > MAX_GROUP_COUNT)
            return table;
        const uintptr_t rows = read_qword_safe(mgr + ITEMINFO_PTR_ARRAY_OFFSET, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{rows}))
            return table;

        // Accessory groups no rule claimed, collected for the report below. Reserved up front so the loop's
        // emplace_back cannot allocate: this function is noexcept, and a throw from inside the walk kills the process.
        std::vector<std::string> unmatched;
        try
        {
            unmatched.reserve(UNMATCHED_GROUP_REPORT_CAP);
            table.resize(count);
        }
        catch (...)
        {
            return table;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const uintptr_t row = read_qword_safe(rows + i * 8ull, ok);
            if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{row}))
                continue;
            const auto name = read_group_name(row);
            if (name.empty())
                continue;
            const auto mapped = slot_from_group_name(name);
            if (mapped.priority == GROUP_PRIORITY_NONE)
            {
                // Report the accessory groups no rule claimed. When a patch renames or re-tiers a group, the only
                // visible symptom is a slot bucket that quietly classifies zero items and whose items then inherit
                // some other slot's type code - which is how necklaces reach the earring bucket. A guess at the new
                // name from strings found elsewhere in memory does not work. The registry the classifier actually
                // reads is the only authority, so print what it holds. Accessory groups only, deduped and capped:
                // consumables and quest items that are SUPPOSED to be unmapped dominate the unmapped set.
                if (unmatched.size() < UNMATCHED_GROUP_REPORT_CAP && icontains(name, "Accessor") &&
                    std::find(unmatched.begin(), unmatched.end(), name) == unmatched.end())
                    unmatched.emplace_back(name);
                continue;
            }
            table[i] = mapped;
            ++mapped_out;
        }
        if (!unmatched.empty())
        {
            // Fail closed on the report, never on the table: the caller needs the classification far more than it
            // needs the diagnostic, and this function is noexcept.
            try
            {
                std::string joined;
                for (const auto &n : unmatched)
                    joined += (joined.empty() ? "" : ", ") + n;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "[catalog-slots] {} accessory group(s) matched NO rule - items in them fall back to another "
                    "slot's type code. Add tails to TAXONOMY_TAIL_RULES/GROUP_SUFFIX_RULES for: {}",
                    unmatched.size(),
                    joined
                );
            }
            catch (...)
            {
            }
        }
        return table;
    }

    /**
     * @brief Classify one item from its group membership.
     * @return The slot, or Count when the item belongs to no mapped group. Count is the normal answer for
     *         consumables, quest items and non-player equipment.
     */
    static TransmogSlot slot_from_item_groups(uintptr_t desc_ptr, const std::vector<GroupSlot> &group_slots) noexcept
    {
        bool ok = false;
        const uintptr_t data = read_qword_safe(desc_ptr + DESC_ITEM_GROUP_DATA_OFFSET, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{data}))
            return TransmogSlot::Count;
        const uint32_t count = read_u32_safe(desc_ptr + DESC_ITEM_GROUP_COUNT_OFFSET, ok);
        if (!ok || count == 0 || count > MAX_ITEM_GROUPS_PER_ITEM)
            return TransmogSlot::Count;

        GroupSlot best;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint16_t value = read_u16_safe(data + i * 2ull, ok);
            if (!ok || value == 0) // 0 is the deserializer's "key not resolved" filler
                continue;
            const std::size_t index = value - 1u;
            if (index >= group_slots.size())
                continue;
            const auto &candidate = group_slots[index];
            if (candidate.priority < best.priority)
                best = candidate;
        }
        return best.slot;
    }

    // Singleton

    ItemNameTable &ItemNameTable::instance()
    {
        static ItemNameTable s;
        return s;
    }

    // Cached intermediate addresses from the 4-hop chain, resolved once in the first build() call. Keeps retry cost to
    // just the catalog walk (and the null-check on the global holder).
    struct ResolvedChain
    {
        bool resolved = false;
        uintptr_t global_holder = 0; // address of the iteminfo global pointer holder
        uintptr_t item_accessor = 0; // ItemAccessor - IndexedStringA short->hash
    };

    static ResolvedChain &cached_chain()
    {
        static ResolvedChain c;
        return c;
    }

    /**
     * @brief Walk SubTranslator to the iteminfo global pointer holder once and cache the result.
     * @return True on success, which fills cached_chain().global_holder and cached_chain().item_accessor. False on a
     *         fatal decoder mismatch, which the caller must not retry.
     * @note Setup/control-plane only: it scans code pages and formats log lines.
     */
    static bool resolve_chain(uintptr_t sub_translator_addr)
    {
        auto &logger = DMK::log();
        auto &chain = cached_chain();
        if (chain.resolved)
            return true;

        if (!sub_translator_addr)
        {
            (void)logger.try_log(DMK::LogLevel::Warning, "[nametable] SubTranslator not resolved - skipping");
            return false;
        }

        // Locate the call to the item descriptor initializer inside SubTranslator. A fixed offset into the function
        // is not reliable, because a compiler prologue reshuffle drifts that offset between patches. Scan a bounded
        // 0x80-byte window of the function instead.
        //
        // The scan tries two anchor encodings, current first:
        //   NAMETABLE_SUB_TX_RSP_LEA_ANCHOR: 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4C 24 ??
        //             (mov r8d,1 / lea rdx,[rbp+disp8] / lea rcx,[rsp+disp8])
        //             The second lea is rsp-relative, so it carries a SIB
        //             byte and is one byte longer than the older form.
        //   NAMETABLE_SUB_TX_RBP_LEA_ANCHOR: 41 B8 01 00 00 00 48 8D 55 ?? 48 8D 4D ??
        //             (mov r8d,1 / lea rdx,[rbp+disp8] / lea rcx,[rbp+disp8])
        //             The second lea is rbp-relative and has no SIB byte.
        //
        // Both disp8 slots carry a wildcard so a future stack-frame shift inside the same function does not require
        // another anchor variant. The 0x80-byte window keeps a stray match elsewhere in .text from leaking in.
        // The window is this function's prologue, so the sweep is bounded to it and global uniqueness never
        // matters. Pages::Executable keeps a byte twin in a data page from being considered at all.
        const DMK::Region sub_tx_window{DMK::Address{sub_translator_addr}, 0x80};

        auto match1 =
            DMK::scan::scan(Transmog::NAMETABLE_SUB_TX_RSP_LEA_ANCHOR, sub_tx_window, 1, DMK::scan::Pages::Executable);
        if (!match1)
        {
            match1 = DMK::scan::scan(
                Transmog::NAMETABLE_SUB_TX_RBP_LEA_ANCHOR,
                sub_tx_window,
                1,
                DMK::scan::Pages::Executable
            );
        }
        if (!match1)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[nametable] descriptor-initializer call anchor not found within the SubTranslator prologue: {}",
                match1.error().message()
            );
            return false;
        }

        // The `|` marker points at the `E8`, so the match IS the call instruction: disp32 at +1, total length 5.
        // resolve_rip_relative reads the displacement under a fault guard and rejects an implausible target.
        const auto desc_init_addr = DMK::scan::resolve_rip_relative(*match1, 1, 5);
        if (!desc_init_addr)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[nametable] descriptor-initializer call at 0x{:X} did not resolve: {}",
                match1->raw(),
                desc_init_addr.error().message()
            );
            return false;
        }
        const uintptr_t desc_init = desc_init_addr->raw();

        // The first `E8` call inside the item descriptor initializer targets ItemAccessor. The DMK sweep skips a
        // coincidental 0xE8 whose disp32 lands on an implausible or unreadable target, then continues the sweep, and
        // it names the concrete failure rather than a silent zero for every miss.
        const auto accessor_addr = DMK::scan::find_and_resolve_rip_relative(
            DMK::Region{DMK::Address{desc_init}, 0x180},
            DMK::scan::PREFIX_CALL_REL32,
            5
        );
        if (!accessor_addr)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[nametable] no rel-call found inside the descriptor initializer: {}",
                accessor_addr.error().message()
            );
            return false;
        }
        const uintptr_t item_accessor = accessor_addr->raw();

        // Locate the `mov rbx, cs:<iteminfo holder>` inside ItemAccessor. A fixed offset is not reliable here either,
        // so scan instead. A distinctive 9-byte prologue-tail anchor precedes the `48 8B 1D disp32`
        //   41 56 48 83 EC ?? 0F B7 39
        // (push r14 / sub rsp,imm8 / movzx edi,word ptr [rcx]) which pins the specific call site inside a bounded
        // 0x40-byte scan of THIS function. Global uniqueness does not matter, because the scan is locally bounded.
        // The stack-alloc imm8 carries a wildcard, because it changes with the frame size (see
        // NAMETABLE_ITEM_ACCESSOR_ANCHOR).
        const auto match3 = DMK::scan::scan(
            Transmog::NAMETABLE_ITEM_ACCESSOR_ANCHOR,
            DMK::Region{DMK::Address{item_accessor}, 0x40},
            1,
            DMK::scan::Pages::Executable
        );
        if (!match3)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[nametable] mov-rbx anchor not found within the ItemAccessor prologue: {}",
                match3.error().message()
            );
            return false;
        }
        // The `|` marker points at the start of the `48 8B 1D disp32` instruction: disp32 at +3, total length 7.
        const auto holder = DMK::scan::resolve_rip_relative(*match3, 3, 7);
        if (!holder)
        {
            (void)logger.try_log(
                DMK::LogLevel::Warning,
                "[nametable] mov-rbx at 0x{:X} did not resolve: {}",
                match3->raw(),
                holder.error().message()
            );
            return false;
        }
        chain.global_holder = holder->raw();
        chain.item_accessor = item_accessor;
        chain.resolved = true;
        logger.info(
            "[nametable] chain resolved: iteminfo holder = 0x{:X}, ItemAccessor = 0x{:X}",
            chain.global_holder,
            chain.item_accessor
        );
        return true;
    }

    uintptr_t ItemNameTable::indexed_string_lookup_addr() const noexcept
    {
        return cached_chain().item_accessor;
    }

    uintptr_t ItemNameTable::iteminfo_holder_addr() const noexcept
    {
        return cached_chain().global_holder;
    }

    ItemNameTable::CatalogInfo ItemNameTable::catalog_info() const noexcept
    {
        CatalogInfo info;
        const auto &chain = cached_chain();
        if (!chain.resolved)
            return info;
        bool ok = false;
        const uintptr_t global_ptr = read_qword_safe(chain.global_holder, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{global_ptr}))
            return info;
        info.count = read_u32_safe(global_ptr + ITEMINFO_COUNT_OFFSET, ok);
        if (!ok || info.count == 0 || info.count > MAX_CATALOG_SIZE)
        {
            info.count = 0;
            return info;
        }
        info.ptr_array = read_qword_safe(global_ptr + ITEMINFO_PTR_ARRAY_OFFSET, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{info.ptr_array}))
        {
            info.ptr_array = 0;
            info.count = 0;
        }
        return info;
    }

    uintptr_t ItemNameTable::descriptor_of(uint16_t item_id) const noexcept
    {
        const auto ci = catalog_info();
        if (ci.ptr_array == 0 || item_id >= ci.count)
            return 0;
        bool ok = false;
        const uintptr_t desc = read_qword_safe(ci.ptr_array + static_cast<uint64_t>(item_id) * 8, ok);
        return (ok && DMK::memory::is_plausible_ptr(DMK::Address{desc})) ? desc : 0;
    }

    ItemNameTable::BuildResult ItemNameTable::build(uintptr_t sub_translator_addr)
    {
        auto &logger = DMK::log();

        // Resolve and cache the address chain. A decoder mismatch is fatal, because retries do not help.
        if (!resolve_chain(sub_translator_addr))
            return BuildResult::Fatal;

        const uintptr_t global_holder = cached_chain().global_holder;

        // Dereference the holder. A null value defers, because the game can still build the iteminfo container.
        bool ok = false;
        const uintptr_t global_ptr = read_qword_safe(global_holder, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{global_ptr}))
        {
            logger.trace(
                "[nametable] iteminfo global not initialized (holder=0x{:X} value=0x{:X}) - deferring",
                global_holder,
                global_ptr
            );
            return BuildResult::Deferred;
        }

        const uint32_t count = read_u32_safe(global_ptr + ITEMINFO_COUNT_OFFSET, ok);
        if (!ok || count == 0 || count > MAX_CATALOG_SIZE)
        {
            logger.trace("[nametable] catalog count implausible: {} (globalPtr=0x{:X}) - deferring", count, global_ptr);
            return BuildResult::Deferred;
        }

        const uintptr_t ptr_array = read_qword_safe(global_ptr + ITEMINFO_PTR_ARRAY_OFFSET, ok);
        if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{ptr_array}))
        {
            logger.trace(
                "[nametable] iteminfo ptrArray null (globalPtr=0x{:X} ptrArray=0x{:X}) - deferring",
                global_ptr,
                ptr_array
            );
            return BuildResult::Deferred;
        }

        // Resolve the ItemGroupInfo registry and turn it into `defIndex -> slot`. Slot classification depends on it
        // entirely, so a miss defers rather than publishes a catalog in which every item reads as non-equipment. The
        // holder address stays fixed for the process, so the probe runs once.
        static uintptr_t s_group_holder = 0;
        if (s_group_holder == 0)
        {
            s_group_holder = probe_group_registry_holder(global_holder);
            if (s_group_holder == 0)
            {
                logger.trace(
                    "[nametable] ItemGroupInfo registry not found near the iteminfo holder (0x{:X}) - deferring",
                    global_holder
                );
                return BuildResult::Deferred;
            }
            logger.info(
                "[nametable] ItemGroupInfo holder = 0x{:X} (iteminfo holder {:+#x})",
                s_group_holder,
                static_cast<std::ptrdiff_t>(s_group_holder - global_holder)
            );
        }

        std::size_t mapped_groups = 0;
        const auto group_slots = build_group_slot_table(s_group_holder, mapped_groups);
        if (group_slots.empty() || mapped_groups == 0)
        {
            logger.trace(
                "[nametable] ItemGroupInfo registry empty or unnamed ({} rows, {} mapped) - deferring",
                group_slots.size(),
                mapped_groups
            );
            return BuildResult::Deferred;
        }

        logger.info(
            "[nametable] scanning item catalog: count={} "
            "globalPtr=0x{:X} ptrArray=0x{:X} (item groups: {} rows, {} mapped to slots)",
            count,
            global_ptr,
            ptr_array,
            group_slots.size(),
            mapped_groups
        );

        const auto t0 = std::chrono::steady_clock::now();

        // Build into local maps first so the published snapshot is atomic from any reader's viewpoint. Only copy into
        // the member maps under the mutex once walking is done.
        std::unordered_map<uint16_t, std::string> id_to_name;
        decltype(m_name_to_id) name_to_id;
        std::unordered_map<uint16_t, uint8_t> variant_flag;
        std::unordered_map<uint16_t, TransmogSlot> slot_map;
        id_to_name.reserve(count);
        name_to_id.reserve(count);
        variant_flag.reserve(count);
        slot_map.reserve(count);

        // Walk the catalog and collect the name, the variant-meta pointer and the transmog slot for every valid
        // descriptor. The variant flag cannot resolve yet, because the sentinel derives statistically from the values
        // this walk collects.
        struct ScratchEntry
        {
            uint16_t id;
            std::string name;
            uintptr_t meta_ptr; // 0 on read fault
            TransmogSlot slot;  // from the item's group membership, Count when its groups name no slot
            uint16_t type_code; // join key for the learned pass below
        };
        std::vector<ScratchEntry> scratch;
        scratch.reserve(count);

        std::size_t valid = 0;
        std::size_t collisions = 0;
        char buf[MAX_NAME_LEN + 1];

        for (uint32_t id = 0; id < count; ++id)
        {
            const uintptr_t desc_ptr = read_qword_safe(ptr_array + id * 8ull, ok);
            if (!ok || !DMK::memory::is_plausible_ptr(DMK::Address{desc_ptr}))
                continue;

            // desc_ptr is reused by two downstream reads (meta_ptr and type_code), so it is resolved separately. Only
            // the wrapper to string pointer hops (desc_ptr -> +0x8 -> +0x0) are folded into one guarded walk.
            static constexpr DMK::memory::ChainStep wrapper_to_string[] = {{0x8}, {0x0}};
            const auto str_slot = DMK::memory::walk(DMK::Address{desc_ptr}, wrapper_to_string);
            if (!str_slot)
                continue;
            const auto str_ptr_opt = DMK::memory::read<uintptr_t>(*str_slot);
            if (!str_ptr_opt || !DMK::memory::is_plausible_ptr(DMK::Address{*str_ptr_opt}))
                continue;
            const uintptr_t str_ptr = *str_ptr_opt;

            const auto len = read_cstring_safe(str_ptr, buf, sizeof(buf));
            if (len == 0 || len >= MAX_NAME_LEN)
                continue;

            const auto id16 = static_cast<uint16_t>(id);
            const uintptr_t meta_ptr = read_qword_safe(desc_ptr + DESC_VARIANT_META_OFFSET, ok);

            // The walk does NOT read wearer-body classification. The rule list at desc+0x248 carries no usable body
            // class, because a game update re-keyed those tokens. Body comes from the equip-eligibility
            // ("Male"/"Female") column of the display_names TSV (see
            // load_display_names / m_body_by_name), applied at query time in sorted_entries and is_player_compatible.
            // The body-table generator (kept out of tree) fills that column from the packed gamedata.
            //
            // Transmog slot from the item's own group membership. See the slot-classification block at the top of this
            // file: the item names the ItemGroupInfo rows it belongs to, and those rows carry the engine's equipment
            // taxonomy by NAME. Anything not in a mapped group (consumables, quest items, pet and mount gear) resolves
            // to Count and stays out of every picker.
            // Items whose groups name no slot (NPC and boss gear, which files under Armor_Mon) stay Count here and are
            // resolved by the learned type-code pass after the walk.
            const TransmogSlot slot = slot_from_item_groups(desc_ptr, group_slots);

            bool tc_ok = false;
            const uint16_t type_code = read_u16_safe(desc_ptr + DESC_TYPE_CODE_OFFSET, tc_ok);

            scratch.push_back({
                id16,
                std::string(buf, len),
                ok ? meta_ptr : 0,
                slot,
                tc_ok ? type_code : TYPE_CODE_NONE,
            });
            ++valid;
        }

        // Derive the variant-meta sentinel statistically. The sentinel is the value that appears at
        // desc+DESC_VARIANT_META_OFFSET in the clear majority of items. Any other pointer at that slot is per-item
        // variant metadata and gates the item out of runtime transmog. Tally the non-zero meta_ptr values, and the
        // mode is the sentinel.
        uintptr_t resolved_sentinel = 0;
        std::size_t sentinel_count = 0;
        {
            std::unordered_map<uintptr_t, std::size_t> tally;
            tally.reserve(64);
            for (const auto &e : scratch)
            {
                if (e.meta_ptr != 0)
                    ++tally[e.meta_ptr];
            }
            for (const auto &kv : tally)
            {
                if (kv.second > sentinel_count)
                {
                    sentinel_count = kv.second;
                    resolved_sentinel = kv.first;
                }
            }
            // Require the mode to dominate - at least 1/3 of valid items must point at it. Below that threshold the
            // data is garbage, and the builder must not flag anything as variant.
            if (valid == 0 || sentinel_count * 3 < valid)
            {
                logger.debug(
                    "[nametable] variant sentinel not dominant "
                    "(best=0x{:X} count={}/{}) - disabling variant-meta filter",
                    resolved_sentinel,
                    sentinel_count,
                    valid
                );
                resolved_sentinel = 0;
            }
            else
            {
                logger.info(
                    "[nametable] variant-meta sentinel resolved: 0x{:X} ({} of {} items)",
                    resolved_sentinel,
                    sentinel_count,
                    valid
                );
            }
        }

        // Learn `type_code -> slot` from the items the group taxonomy classified.
        //
        // The group names carry the slot only for PLAYER equipment. NPC and boss gear sits in families like
        // ItemGroup_Equip_Armor_Mon that name no slot, so groups alone leave roughly two thirds of the wearable
        // catalog unclassified. The type code closes that gap: a boss helm shares its code with player helms.
        //
        // Majority vote rather than first-wins, because a handful of items carry a sub-category that disagrees with
        // their code (one item votes Helm for the chest code). Those lose by two orders of magnitude. A contested code
        // reaches the log rather than the floor: a code that splits evenly means the join is no longer sound.
        std::unordered_map<uint16_t, TransmogSlot> learned_slot;
        std::size_t contested_codes = 0;
        {
            std::unordered_map<uint16_t, std::unordered_map<TransmogSlot, uint32_t>> votes;
            for (const auto &e : scratch)
            {
                if (e.slot != TransmogSlot::Count && e.type_code != TYPE_CODE_NONE)
                    ++votes[e.type_code][e.slot];
            }

            learned_slot.reserve(votes.size());
            for (const auto &[code, tally] : votes)
            {
                TransmogSlot winner = TransmogSlot::Count;
                uint32_t top_votes = 0;
                uint32_t total = 0;
                for (const auto &[slot, n] : tally)
                {
                    total += n;
                    if (n > top_votes)
                    {
                        top_votes = n;
                        winner = slot;
                    }
                }
                if (top_votes < total)
                {
                    ++contested_codes;
                    logger.trace(
                        "[catalog-slots] type code {:#06x} contested: {} of {} votes for {}",
                        code,
                        top_votes,
                        total,
                        slot_name(winner)
                    );
                }
                learned_slot.emplace(code, winner);
            }
        }

        // Publish the scratch rows into the maps and flag variants against the resolved sentinel.
        std::size_t variant_count = 0;
        for (auto &e : scratch)
        {
            // Item "has variant" (picker shows carrier-color) when desc+DESC_VARIANT_META_OFFSET is non-sentinel. That
            // value is a per-item variant-meta record threaded through the catalog list.
            //
            // A ">= 2 body-bearing classifier rules" heuristic does NOT work as a substitute. The reshaped rule struct
            // gives nearly every item the same large rule count, so that heuristic over-flags and yields hundreds of
            // false positives. The variant-meta pointer alone is the reliable signal. See DESC_VARIANT_META_OFFSET for
            // how to re-derive the offset.
            const bool has_variant = (resolved_sentinel != 0) && (e.meta_ptr != 0) && (e.meta_ptr != resolved_sentinel);
            if (has_variant)
                ++variant_count;

            id_to_name.emplace(e.id, e.name);
            auto [it, inserted] = name_to_id.emplace(std::move(e.name), e.id);
            if (!inserted)
                ++collisions;
            variant_flag.emplace(e.id, has_variant ? uint8_t{1} : uint8_t{0});

            TransmogSlot slot = e.slot;
            if (slot == TransmogSlot::Count && e.type_code != TYPE_CODE_NONE)
            {
                if (auto lit = learned_slot.find(e.type_code); lit != learned_slot.end())
                    slot = lit->second;
            }
            if (slot != TransmogSlot::Count)
                slot_map.emplace(e.id, slot);
        }

        // Stability check: the game sets the iteminfo count to its final value early, but it populates the descriptor
        // pointer array lazily. A fixed "good enough" percentage gate is not reliable, because it lets a partial
        // catalog through. Instead, wait until two consecutive scans produce the same valid count, which means the
        // array stopped growing. This self-adapts to any catalog size and any game version.
        if (valid == 0)
        {
            logger.trace("[nametable] no valid descriptors - deferring");
            m_last_build_valid = 0;
            return BuildResult::Deferred;
        }
        if (valid != m_last_build_valid)
        {
            logger.trace("[nametable] catalog still loading ({} -> {} valid) - deferring", m_last_build_valid, valid);
            m_last_build_valid = static_cast<uint32_t>(valid);
            return BuildResult::Deferred;
        }
        // valid > 0 && valid == m_last_build_valid -> catalog stabilized.

        // Per-slot histogram of the classification, with sample names. A patch that reshapes the group registry or
        // renames a sub-category shows up here as a slot that went to zero, which is the failure this classifier
        // makes visible. A type-code table fails silently instead, by listing the WRONG items.
        {
            std::unordered_map<TransmogSlot, std::vector<std::uint16_t>> bucket;
            bucket.reserve(static_cast<std::size_t>(TransmogSlot::Count));
            for (const auto &kv : slot_map)
                bucket[kv.second].push_back(kv.first);

            logger.trace(
                "[catalog-slots] {}/{} items classified across {} slots "
                "({} type codes learned from group names, {} contested)",
                slot_map.size(),
                valid,
                bucket.size(),
                learned_slot.size(),
                contested_codes
            );

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
                    auto nit = id_to_name.find(ids[k]);
                    if (k > 0)
                        samples += ", ";
                    samples += (nit != id_to_name.end()) ? nit->second : "<unknown>";
                }

                logger.trace("[catalog-slots]   {:<13} count={:>4} samples: {}", slot_name(slot), ids.size(), samples);
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_id_to_name = std::move(id_to_name);
            m_name_to_id = std::move(name_to_id);
            m_variant_flag = std::move(variant_flag);
            m_slot_by_id = std::move(slot_map);
            m_sorted_cache.reset(); // rebuilt lazily on the next sorted_entries()
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        logger.info(
            "[nametable] built: {}/{} entries ({} name collisions, {} variant-meta) in {}ms",
            valid,
            count,
            collisions,
            variant_count,
            ms
        );
        return BuildResult::Ok;
    }

    std::string ItemNameTable::name_of(uint16_t item_id) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_id_to_name.find(item_id);
        if (it == m_id_to_name.end())
            return {};
        return it->second;
    }

    std::optional<uint16_t> ItemNameTable::id_of(std::string_view name) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_name_to_id.find(name);
        if (it == m_name_to_id.end())
            return std::nullopt;
        return it->second;
    }

    bool ItemNameTable::has_variant_meta(uint16_t item_id) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_variant_flag.find(item_id);
        return it != m_variant_flag.end() && it->second != 0;
    }

    bool ItemNameTable::is_player_compatible(uint16_t item_id) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // Kliff-centric: safe to bind on the (male) player unless the item is restricted to the female body. Body is
        // sourced from the display_names equip-eligibility column (m_body_by_name, keyed by lowercase internal name).
        auto nit = m_id_to_name.find(item_id);
        if (nit == m_id_to_name.end())
            return true; // unknown -> prefer to surface
        const std::string key = fold_key(nit->second);
        auto bit = m_body_by_name.find(key);
        return bit == m_body_by_name.end() || bit->second != BodyKind::Female;
    }

    ItemNameTable::BodyKind ItemNameTable::body_kind_for_item(uint16_t item_id) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // Lowercase the internal name to match m_body_by_name's keys. An item wearable by both bodies (or
        // unrestricted) is absent from the map and resolves to BodyKind::Generic.
        auto nit = m_id_to_name.find(item_id);
        if (nit == m_id_to_name.end())
            return BodyKind::Generic;
        const std::string key = fold_key(nit->second);
        auto bit = m_body_by_name.find(key);
        return bit != m_body_by_name.end() ? bit->second : BodyKind::Generic;
    }

    TransmogSlot ItemNameTable::category_of(uint16_t item_id) const noexcept
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        // Runtime-observed binding wins. If the engine actually equipped this item_id in a slot, that is ground truth
        // and beats the catalog classification.
        if (auto obs = m_observed_slot.find(item_id); obs != m_observed_slot.end())
            return obs->second;

        auto it = m_slot_by_id.find(item_id);
        return (it != m_slot_by_id.end()) ? it->second : TransmogSlot::Count;
    }

    TransmogSlot ItemNameTable::catalog_category_of(uint16_t item_id) const noexcept
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_slot_by_id.find(item_id);
        return (it != m_slot_by_id.end()) ? it->second : TransmogSlot::Count;
    }

    void ItemNameTable::record_observed_slot(std::uint16_t item_id, TransmogSlot slot) noexcept
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (slot == TransmogSlot::Count)
        {
            m_observed_slot.erase(item_id);
            return;
        }
        m_observed_slot[item_id] = slot;
    }

    std::size_t ItemNameTable::observed_slot_count() const noexcept
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_observed_slot.size();
    }

    ItemNameTable::BodyKind ItemNameTable::body_kind_for_character(std::string_view char_name) noexcept
    {
        // Fixed per-character body: Kliff and Oongka use the male humanoid skeleton, Damiane the female one. Unknown
        // characters default to Generic. Their body is not known, so treat every item as potentially compatible
        // rather than silently hiding their picker.
        if (char_name == "Kliff" || char_name == "Oongka")
            return BodyKind::Male;
        if (char_name == "Damiane")
            return BodyKind::Female;
        return BodyKind::Generic;
    }

    std::shared_ptr<const std::vector<ItemNameTable::Entry>> ItemNameTable::sorted_entries() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_sorted_cache != nullptr)
            return m_sorted_cache;
        if (m_id_to_name.empty())
            return std::make_shared<const std::vector<Entry>>();

        // Build into a local, then publish. Nothing observes a half-sorted list, and the published vector is never
        // mutated again, which is what lets a holder keep reading it after a retire.
        std::vector<Entry> built;
        built.reserve(m_id_to_name.size());
        for (const auto &[id, name] : m_id_to_name)
        {
            auto vit = m_variant_flag.find(id);
            const bool has_variant = (vit != m_variant_flag.end()) && (vit->second != 0);

            // Lowercased internal name keys both the display-name and the wearer-body maps (both loaded from the
            // display_names TSV).
            const std::string lower_name = fold_key(name);

            // Wearer-body classification comes from the equip-eligibility column of the display_names TSV
            // (m_body_by_name). Only single-body-restricted items are listed. Anything wearable by both bodies (or
            // unrestricted) is absent -> BodyKind::Generic, shown on every character. The body-table generator (kept
            // out of tree) fills the column.
            auto brit = m_body_by_name.find(lower_name);
            const BodyKind kind = (brit != m_body_by_name.end()) ? brit->second : BodyKind::Generic;
            // Kliff-centric "PlayerSafe": an item is player-safe unless it is restricted to the female body.
            const bool is_player = (kind != BodyKind::Female);

            auto dit = m_display_names.find(lower_name);
            std::string disp_name = (dit != m_display_names.end()) ? dit->second : std::string();

            auto sit = m_search_names.find(lower_name);
            std::string search_name = (sit != m_search_names.end()) ? sit->second : std::string();

            // The item's group membership is authoritative. Anything with no mapped group (pet and mount gear, quest
            // items, consumables) is absent from the map and collapses to Count, hiding it as non-equipment. There is
            // no name-parsing fallback: the groups ARE the engine's classification.
            auto slit = m_slot_by_id.find(id);
            const TransmogSlot slot = (slit != m_slot_by_id.end()) ? slit->second : TransmogSlot::Count;

            built.push_back({
                id,
                slot,
                has_variant,
                is_player,
                kind,
                name,
                std::move(disp_name),
                std::move(search_name),
            });
        }

        std::sort(
            built.begin(),
            built.end(),
            [](const Entry &a, const Entry &b)
            {
                // Sort by display name when available, else by internal name. Case-insensitive so "Kliff" and
                // "kliff" sort together.
                const auto &sa = a.display_name.empty() ? a.name : a.display_name;
                const auto &sb = b.display_name.empty() ? b.name : b.display_name;
                const std::size_t n = (std::min)(sa.size(), sb.size());
                for (std::size_t i = 0; i < n; ++i)
                {
                    const auto ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(sa[i])));
                    const auto cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(sb[i])));
                    if (ca != cb)
                        return ca < cb;
                }
                return sa.size() < sb.size();
            }
        );

        m_sorted_cache = std::make_shared<const std::vector<Entry>>(std::move(built));
        return m_sorted_cache;
    }

    void ItemNameTable::dump_catalog_tsv() const
    {
        auto &logger = DMK::log();

        std::wstring rt_dir = DMK::filesystem::get_runtime_directory();
        if (rt_dir.empty())
        {
            logger.warning("[nametable] dump_catalog_tsv: runtime dir unavailable");
            return;
        }
        if (rt_dir.back() != L'\\' && rt_dir.back() != L'/')
            rt_dir.push_back(L'\\');

        std::wstring path = rt_dir + L"CrimsonDesertLiveTransmog_items.tsv";

        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            logger.warning("[nametable] dump_catalog_tsv: failed to open output file");
            return;
        }

        const auto entries = sorted_entries();

        out << "ItemID\tSlot\tVariant\tPlayerSafe\tName\n";
        for (const auto &e : *entries)
        {
            const char *slot_str = "Other";
            if (e.category != TransmogSlot::Count)
                slot_str = slot_name(e.category);

            out << "0x" << std::hex << std::uppercase << e.id << std::dec << '\t' << slot_str << '\t'
                << (e.has_variant_meta ? "yes" : "no") << '\t' << (e.is_player_compatible ? "yes" : "no") << '\t'
                << e.name << '\n';
        }

        logger.info("[nametable] dumped {} entries to CrimsonDesertLiveTransmog_items.tsv", entries->size());
    }

    // Trim ASCII whitespace and any line terminator from both ends of one field. A TSV written with trailing padding
    // would otherwise produce a key that never matches or a body column that reads as unrestricted.
    static std::string_view trim_field(std::string_view field) noexcept
    {
        constexpr auto is_space = [](char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!field.empty() && is_space(field.front()))
            field.remove_prefix(1);
        while (!field.empty() && is_space(field.back()))
            field.remove_suffix(1);
        return field;
    }

    /**
     * @brief Split one row on tabs into at most three trimmed fields.
     * @return The number of fields the row carries.
     */
    static std::size_t split_row(std::string_view line, std::array<std::string_view, 3> &fields) noexcept
    {
        std::size_t count = 0;
        std::size_t pos = 0;
        while (count < fields.size())
        {
            const auto tab = line.find('\t', pos);
            const auto end = (tab == std::string_view::npos) ? line.size() : tab;
            fields[count++] = trim_field(line.substr(pos, end - pos));
            if (tab == std::string_view::npos)
                break;
            pos = tab + 1;
        }
        return count;
    }

    /// Invoke @p fn once per non-empty newline-delimited row of @p payload.
    template <class Fn> static void for_each_row(std::string_view payload, Fn &&fn)
    {
        std::size_t pos = 0;
        while (pos < payload.size())
        {
            const auto newline = payload.find('\n', pos);
            const auto end = (newline == std::string_view::npos) ? payload.size() : newline;
            if (const auto line = payload.substr(pos, end - pos); !line.empty())
                fn(line);
            if (newline == std::string_view::npos)
                break;
            pos = newline + 1;
        }
    }

    /**
     * @brief Ingest `internal_name<TAB>display_name` rows, replacing any key the payload names.
     * @return The number of rows applied.
     */
    static std::size_t apply_name_rows(std::string_view payload, std::unordered_map<std::string, std::string> &names)
    {
        std::size_t applied = 0;
        for_each_row(
            payload,
            [&](std::string_view line)
            {
                std::array<std::string_view, 3> fields{};
                if (split_row(line, fields) < 2 || fields[0].empty() || fields[1].empty())
                    return;
                names[fold_key(fields[0])] = std::string{fields[1]};
                ++applied;
            }
        );
        return applied;
    }

    /**
     * @brief Ingest `internal_name<TAB>Male|Female` rows into the wearer-body map.
     * @return The number of rows applied.
     */
    static std::size_t
    apply_body_rows(std::string_view payload, std::unordered_map<std::string, ItemNameTable::BodyKind> &body)
    {
        std::size_t applied = 0;
        for_each_row(
            payload,
            [&](std::string_view line)
            {
                std::array<std::string_view, 3> fields{};
                if (split_row(line, fields) < 2 || fields[0].empty())
                    return;
                if (fields[1] == "Male")
                    body[fold_key(fields[0])] = ItemNameTable::BodyKind::Male;
                else if (fields[1] == "Female")
                    body[fold_key(fields[0])] = ItemNameTable::BodyKind::Female;
                else
                    return;
                ++applied;
            }
        );
        return applied;
    }

    /**
     * @brief Ingest the user override TSV over both maps.
     * @return The number of rows that changed a display name or a body value.
     *
     * @details Columns are `<internal name> \t <display name> [\t Male|Female]`. Each field is optional past the key:
     *          an empty display name leaves the published name intact, and an absent third column leaves the published
     *          body intact. The overlay never erases a row the file omits.
     */
    static std::size_t apply_override_rows(
        std::string_view payload,
        std::unordered_map<std::string, std::string> &names,
        std::unordered_map<std::string, ItemNameTable::BodyKind> &body
    )
    {
        std::size_t applied = 0;
        for_each_row(
            payload,
            [&](std::string_view line)
            {
                std::array<std::string_view, 3> fields{};
                const auto count = split_row(line, fields);
                if (count < 2 || fields[0].empty())
                    return;

                const std::string key = fold_key(fields[0]);
                bool changed = false;
                if (!fields[1].empty())
                {
                    names[key] = std::string{fields[1]};
                    changed = true;
                }
                if (count > 2 && fields[2] == "Male")
                {
                    body[key] = ItemNameTable::BodyKind::Male;
                    changed = true;
                }
                else if (count > 2 && fields[2] == "Female")
                {
                    body[key] = ItemNameTable::BodyKind::Female;
                    changed = true;
                }
                if (changed)
                    ++applied;
            }
        );
        return applied;
    }

    void ItemNameTable::load_display_names(std::string_view locale_tag, const std::filesystem::path &tsv_path)
    {
        auto &logger = DMK::log();

        std::unordered_map<std::string, std::string> names;
        std::unordered_map<std::string, std::string> search_names;
        std::unordered_map<std::string, BodyKind> body_by_name;
        names.reserve(7000);

        const auto &pack = LanguagePack::instance();
        const bool pack_ready = pack.ready();
        bool pack_supplied_names = false;
        std::size_t localized = 0;
        if (pack_ready)
        {
            // Layer 1. The pack is the only source of the wearer-body restriction, and the block is locale
            // independent, so it loads once regardless of the selected locale.
            if (const auto block = pack.read_block("body", ""); block.has_value())
                apply_body_rows(*block, body_by_name);

            // Layer 2. English is always the baseline, so a locale that omits an item still shows a readable name.
            if (const auto block = pack.read_block("names", "eng"); block.has_value())
                (void)apply_name_rows(*block, names);

            // Layer 3. Display names only. It must not touch the body map.
            if (!locale_tag.empty() && locale_tag != "eng")
            {
                if (const auto block = pack.read_block("names", locale_tag); block.has_value())
                    localized = apply_name_rows(*block, names);
                else
                    logger.warning("[nametable] locale '{}' unavailable; display names stay English", locale_tag);

                // A "search" block marks its locale as pre-shaped: the names above are presentation forms in visual
                // order, which no typed query can match. The original logical text keeps the picker searchable.
                if (const auto block = pack.read_block("search", locale_tag); block.has_value())
                    (void)apply_name_rows(*block, search_names);
            }

            // Any block that produced a row counts, not the English one alone. A pack that carries a locale and no
            // English baseline must not then have every localized name overwritten by the English TSV below.
            pack_supplied_names = !names.empty();
        }

        // Layer 4. The shipped TSV is a COMPLETE English table, so it is a fallback, never an override. Applying it
        // over a locale would rewrite every name back to English and silently undo the locale choice. It loads only
        // when the pack supplied no names at all, which is the pack-absent install.
        bool fallback_used = false;
        if (!pack_supplied_names)
        {
            if (std::ifstream file{tsv_path, std::ios::binary}; file.is_open())
            {
                fallback_used = true;
                const std::string content{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
                apply_override_rows(content, names, body_by_name);
            }
        }

        // Layer 5. The user override, always applied last. It lives beside the shipped TSV under a distinct name
        // so a user edit is never confused with the shipped table:
        //   <stem>.override.tsv    applies to every locale
        //   <stem>.<tag>.tsv       applies to one locale only
        std::size_t overrides = 0;
        const auto apply_override_file = [&](const std::filesystem::path &path)
        {
            if (std::ifstream file{path, std::ios::binary}; file.is_open())
            {
                const std::string content{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
                overrides += apply_override_rows(content, names, body_by_name);
            }
        };
        {
            // Concatenate onto the path itself. Rebuilding it from stem().string() would round-trip a non-ASCII
            // install path through the ANSI codepage, which is the bug this module avoids everywhere else. The
            // suffixes below are ASCII, so only the directory part carries a non-ASCII risk and it never converts.
            std::filesystem::path base = tsv_path;
            base.replace_extension();

            std::filesystem::path shared = base;
            shared += ".override.tsv";
            apply_override_file(shared);

            if (!locale_tag.empty())
            {
                std::filesystem::path per_locale = base;
                per_locale += ".";
                per_locale += locale_tag;
                per_locale += ".tsv";
                apply_override_file(per_locale);
            }
        }

        if (!pack_supplied_names && !fallback_used)
        {
            // Publishing here would wipe a previously loaded table on a locale switch that found neither source.
            logger.warning("[nametable] no language pack and no display names file at '{}'", to_utf8(tsv_path));
            return;
        }

        const std::size_t name_count = names.size();
        const std::size_t body_count = body_by_name.size();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_display_names = std::move(names);
            m_search_names = std::move(search_names);
            m_body_by_name = std::move(body_by_name);
            // Retire rather than mutate. A picker iterating the previous snapshot on the render thread holds its
            // own reference to it, so the reset here cannot free elements under it, and the next sorted_entries()
            // builds a fresh one carrying the new names.
            m_sorted_cache.reset();
        }

        logger.info(
            "[nametable] {} display names ({} body-restricted), locale '{}' supplied {}, overrides {}, fallback {}",
            name_count,
            body_count,
            locale_tag.empty() ? std::string_view{"eng"} : locale_tag,
            localized,
            overrides,
            fallback_used
        );
    }

    std::string ItemNameTable::display_name_of(std::string_view internal_name) const
    {
        // Lowercase into a stack buffer to avoid heap allocation. Item names in the catalog are bounded by
        // MAX_NAME_LEN, which is smaller than this buffer, and the copy is clamped to the buffer size anyway.
        char buf[256];
        const auto len = (std::min)(internal_name.size(), sizeof(buf) - 1);
        for (std::size_t i = 0; i < len; ++i)
            buf[i] = ascii_lower(internal_name[i]);
        buf[len] = '\0';
        const std::string key{buf, len};

        std::lock_guard<std::mutex> lk(m_mutex);
        const auto it = m_display_names.find(key);
        if (it == m_display_names.end())
            return {};
        return it->second;
    }

} // namespace Transmog
