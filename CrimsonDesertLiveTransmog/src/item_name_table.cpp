#include "item_name_table.hpp"
#include "transmog_map.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace Transmog
{
    // Sensible upper bound on item descriptor catalog size.
    // v1.02.00 ships ~6024 entries; guard generously.
    static constexpr uint32_t k_maxCatalogSize = 0x20000;

    static constexpr std::size_t k_maxNameLen = 96;

    // Variant-metadata detection (see item_name_table.hpp::has_variant_meta).
    // Clean base items have `*(desc+0x3A0) == <sentinel>` where <sentinel>
    // is a shared empty-object pointer (IDA: off_1459D0B38 on v1.02.00 — an
    // IRefCounted vtable in the exe's .data section). Non-sentinel values
    // point to a per-item metadata struct threaded through a catalog-wide
    // linked list. Members of this list failed to render via runtime
    // transmog on all tested samples.
    //
    // The sentinel's RVA is NOT stable across patches — any .data reshuffle
    // moves it. Instead of hardcoding the RVA, we resolve the sentinel
    // statistically at catalog-build time: scan every valid descriptor's
    // +0x3A0 qword, the value that appears in the clear majority of items
    // (historically ~60% on v1.02.00, ~3625/6024) IS the sentinel. This
    // needs no symbol anchor and self-heals across future game updates.
    static constexpr std::ptrdiff_t k_descVariantMetaOffset = 0x3A0;

    // --- iteminfo (qword_145CEF370) container layout, v1.02.00 ---
    // Source: IDA sub_1402D75D0 + static analysis. These are runtime data
    // offsets (not code) so they cannot be AOB-scanned; if a future patch
    // reshapes the struct the catalog walk will produce an implausible
    // count/ptrArray and bail at the existing sanity checks below.
    static constexpr std::ptrdiff_t k_iteminfoCountOffset     = 0x08; // dword entry count
    static constexpr std::ptrdiff_t k_iteminfoPtrArrayOffset  = 0x50; // 80: qword base of descriptor ptr array

    // Resolved sentinel, cached after first successful build().
    // 0 means "not yet resolved" — has_variant_meta() falls back to false
    // (preferring to let a bad item through rather than mis-flag a clean
    // one) until the next build() populates it.
    static std::atomic<uintptr_t> s_variantMetaSentinel{0};

    // --- Safe memory helpers (SEH-wrapped) ---

    static uint8_t read_u8_safe(uintptr_t addr, bool &ok) noexcept
    {
        __try
        {
            uint8_t v = *reinterpret_cast<const uint8_t *>(addr);
            ok = true;
            return v;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
            return 0;
        }
    }

    static int32_t read_i32_safe(uintptr_t addr, bool &ok) noexcept
    {
        __try
        {
            int32_t v = 0;
            std::memcpy(&v, reinterpret_cast<const void *>(addr), sizeof(v));
            ok = true;
            return v;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
            return 0;
        }
    }

    static uintptr_t read_qword_safe(uintptr_t addr, bool &ok) noexcept
    {
        __try
        {
            uintptr_t v = *reinterpret_cast<const uintptr_t *>(addr);
            ok = true;
            return v;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
            return 0;
        }
    }

    static uint32_t read_u32_safe(uintptr_t addr, bool &ok) noexcept
    {
        __try
        {
            uint32_t v = *reinterpret_cast<const uint32_t *>(addr);
            ok = true;
            return v;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
            return 0;
        }
    }

    // --- Player-compatibility detection via rule-classifier tokens ---
    //
    // Every armor descriptor at `+0x248` holds a stride-0x38 rule list;
    // each rule's classifier hash array at `rule+0x20` (count at
    // `rule+0x28`) lists u16 body-type tokens the game's rule evaluator
    // will accept for that item. On v1.02.00, every item that renders on
    // the player has at least one rule whose classifier list contains a
    // token from the "player body-type" set below. Mount/pet/non-humanoid
    // items (horse tack, wagon gear) use a disjoint classifier set and
    // crash the mesh binder when equipped on the player.
    //
    // CE-verified distribution (v1.02.00):
    //   - 2109 items have >=1 player classifier in their rule list (safe)
    //   - 3184 items have only non-player classifiers (unsafe, hidden)
    //   - 731 items have no rules at all (default: treat as safe — these
    //     are inert cosmetics / quest items, not mount armor)
    //   - 80/80 HorseArmor items correctly flagged unsafe; spot checks
    //     (Wolf_Pursuer, Bear_Chief, Kliff_Helm, Antra_Helm) all correct.
    //
    // If a future patch changes the player classifier set, re-run the
    // catalog differential against known-player vs known-mount items.
    static constexpr std::uint16_t k_playerClassifiers[] = {
        0x0015, 0x0018, 0x0058, 0x02E3, 0x039D,
    };
    static constexpr std::ptrdiff_t k_ruleListPtrOffset    = 0x248;
    static constexpr std::ptrdiff_t k_ruleListCountOffset  = 0x250;
    static constexpr std::size_t    k_ruleStride           = 0x38;
    static constexpr std::ptrdiff_t k_ruleClassPtrOffset   = 0x20;
    static constexpr std::ptrdiff_t k_ruleClassCountOffset = 0x28;
    static constexpr std::uint32_t  k_maxPlausibleRuleCount  = 64;
    static constexpr std::uint32_t  k_maxPlausibleClassCount = 32;

    // Returns true if the item at `descPtr` has at least one rule whose
    // classifier array contains a player body-type token. Items with no
    // rules (or unreadable fields) default to true — we'd rather let a
    // player-safe cosmetic through than hide it by accident.
    static bool item_has_player_classifier(uintptr_t descPtr) noexcept
    {
        bool ok = false;
        const auto rulesPtr = read_qword_safe(
            descPtr + k_ruleListPtrOffset, ok);
        if (!ok || rulesPtr < 0x10000)
            return true;
        const auto ruleCount = read_u32_safe(
            descPtr + k_ruleListCountOffset, ok);
        if (!ok || ruleCount == 0 || ruleCount > k_maxPlausibleRuleCount)
            return true;

        for (std::uint32_t i = 0; i < ruleCount; ++i)
        {
            const auto rule = rulesPtr + i * k_ruleStride;
            const auto clsPtr = read_qword_safe(
                rule + k_ruleClassPtrOffset, ok);
            if (!ok || clsPtr < 0x10000)
                continue;
            const auto clsCount = read_u32_safe(
                rule + k_ruleClassCountOffset, ok);
            if (!ok || clsCount == 0 || clsCount > k_maxPlausibleClassCount)
                continue;

            for (std::uint32_t j = 0; j < clsCount; ++j)
            {
                std::uint16_t cls = 0;
                __try
                {
                    cls = *reinterpret_cast<const std::uint16_t *>(
                        clsPtr + 2ull * j);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    break;
                }
                for (const auto pc : k_playerClassifiers)
                {
                    if (cls == pc)
                        return true;
                }
            }
        }
        return false;
    }

    /**
     * Decode a relative-call instruction ( `E8 disp32` ) at the given
     * address and return its target. Returns 0 on failure.
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
     * Scan the first `scanBytes` of a function for the first `E8 disp32`
     * call and return its target. Returns 0 on failure.
     */
    static uintptr_t first_rel_call_target(uintptr_t funcStart,
                                           std::size_t scanBytes) noexcept
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
    static std::size_t read_cstring_safe(uintptr_t strPtr, char *buf,
                                         std::size_t bufSize) noexcept
    {
        __try
        {
            const auto *src = reinterpret_cast<const char *>(strPtr);
            std::size_t len = 0;
            while (len < bufSize - 1 && src[len] != '\0')
            {
                const auto c = src[len];
                // Accept printable ASCII only — reject if we see control
                // chars (suggests we're reading the wrong pointer).
                if (static_cast<unsigned char>(c) < 0x20 ||
                    static_cast<unsigned char>(c) > 0x7E)
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

    // Strip trailing variant/noise tokens in-place so the tail-match below
    // sees the meaningful last segment. CE-verified noise tokens against
    // the v1.02.00 catalog: _UpgradeN, _IVX (roman), _NN (numeric),
    // _Large/_XLarge/_Medium/_Small, _Pancake.
    static void strip_variant_tail(std::string &s) noexcept
    {
        auto ends_with = [](const std::string &str, const char *suf) {
            const auto n = std::strlen(suf);
            if (str.size() < n)
                return false;
            return std::memcmp(str.data() + str.size() - n, suf, n) == 0;
        };

        for (;;)
        {
            const auto prev = s.size();

            // _UpgradeN? (N optional, 1-3 digits)
            {
                auto pos = s.rfind("_Upgrade");
                if (pos != std::string::npos)
                {
                    bool allDigits = true;
                    for (auto i = pos + 8; i < s.size(); ++i)
                    {
                        if (!std::isdigit(static_cast<unsigned char>(s[i])))
                        {
                            allDigits = false;
                            break;
                        }
                    }
                    if (allDigits)
                        s.resize(pos);
                }
            }

            // _<roman> (single trailing run of I/V/X)
            if (auto pos = s.find_last_of('_'); pos != std::string::npos)
            {
                bool romanOnly = (pos + 1 < s.size());
                for (auto i = pos + 1; i < s.size() && romanOnly; ++i)
                {
                    const char c = s[i];
                    if (c != 'I' && c != 'V' && c != 'X')
                        romanOnly = false;
                }
                if (romanOnly)
                    s.resize(pos);
            }

            // _<digits>
            if (auto pos = s.find_last_of('_'); pos != std::string::npos)
            {
                bool digitsOnly = (pos + 1 < s.size());
                for (auto i = pos + 1; i < s.size() && digitsOnly; ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(s[i])))
                        digitsOnly = false;
                }
                if (digitsOnly)
                    s.resize(pos);
            }

            // Size tags.
            for (const char *suf : {"_Large", "_XLarge", "_Medium", "_Small", "_Pancake"})
            {
                if (ends_with(s, suf))
                {
                    s.resize(s.size() - std::strlen(suf));
                    break;
                }
            }

            if (s.size() == prev)
                break;
        }
    }

    // Returns true if the item name starts with a known non-equipment
    // prefix — crafting recipes, recipe books, knowledge books, etc.
    // These items sometimes end with armor-slot suffixes (e.g.
    // "CraftingRecipe_Oongka_PlateArmor_Helm_III") but are NOT
    // equippable armor. CE-verified: all such items have ruleCount=0
    // and default to isPlayer=true, so this prefix check is the only
    // gate keeping them out of the picker.
    static bool is_non_equipment_prefix(const std::string &name) noexcept
    {
        // Sorted by frequency in v1.02.00 catalog.
        static constexpr const char *k_prefixes[] = {
            "CraftingRecipe_",
            "Recipe_",
            "Book_",
            "Item_Skill_",
        };
        for (const char *pfx : k_prefixes)
        {
            const auto n = std::strlen(pfx);
            if (name.size() >= n &&
                std::memcmp(name.data(), pfx, n) == 0)
                return true;
        }
        return false;
    }

    // Case-insensitive comparison helper for suffix matching.
    static bool tail_eq_ci(const std::string &tail, const char *target) noexcept
    {
        const auto n = std::strlen(target);
        if (tail.size() != n)
            return false;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(tail[i])) !=
                std::tolower(static_cast<unsigned char>(target[i])))
                return false;
        }
        return true;
    }

    TransmogSlot ItemNameTable::classify_slot(const std::string &name) noexcept
    {
        if (name.empty())
            return TransmogSlot::Count;

        // Non-equipment prefixes (recipes, books, skill items) can
        // carry armor-slot suffixes — reject them early so they never
        // classify into a transmog slot.
        if (is_non_equipment_prefix(name))
            return TransmogSlot::Count;

        std::string stripped = name;
        strip_variant_tail(stripped);

        // Find the last underscore and compare the tail. Case-INSENSITIVE —
        // CE-verified: v1.02.00 has 4 items with lowercase "_cloak" that
        // are real equippable cloaks (Paulus_Basic_Leather_cloak, etc).
        const auto dash = stripped.find_last_of('_');
        std::string tail = (dash == std::string::npos)
                               ? stripped
                               : stripped.substr(dash + 1);

        if (tail_eq_ci(tail, "Helm"))
            return TransmogSlot::Helm;
        if (tail_eq_ci(tail, "Armor") || tail_eq_ci(tail, "Cloth"))
            return TransmogSlot::Chest;
        if (tail_eq_ci(tail, "Cloak"))
            return TransmogSlot::Cloak;
        if (tail_eq_ci(tail, "Gloves"))
            return TransmogSlot::Gloves;
        if (tail_eq_ci(tail, "Boots"))
            return TransmogSlot::Boots;

        return TransmogSlot::Count;
    }

    // --- Singleton ---

    ItemNameTable &ItemNameTable::instance()
    {
        static ItemNameTable s;
        return s;
    }

    // Mutex guarding writes to m_idToName/m_nameToId/m_sortedCache during
    // a background-thread publish. Readers (name_of/id_of/sorted_entries)
    // take a shared-ish view via the same mutex; build contention is
    // limited to the handful of picker-popup calls per frame, so a plain
    // std::mutex is fine.
    static std::mutex s_tableMtx;

    // Cached intermediate addresses from the 4-hop chain, resolved once
    // in the first build() call. Keeps retry cost to just the catalog
    // walk (and the null-check on the global holder).
    struct ResolvedChain
    {
        bool resolved = false;
        uintptr_t globalHolder = 0; // &qword_145CEF370
        uintptr_t itemAccessor = 0; // sub_1402D75D0 — IndexedStringA short->hash
    };

    static ResolvedChain &cached_chain()
    {
        static ResolvedChain c;
        return c;
    }

    // Walk sub_14076D950 -> ... -> qword_145CEF370 address once and cache.
    // Returns false on fatal decoder mismatch (do not retry). On success
    // fills cached_chain().globalHolder.
    static bool resolve_chain(uintptr_t subTranslatorAddr) noexcept
    {
        auto &logger = DMK::Logger::get_instance();
        auto &chain = cached_chain();
        if (chain.resolved)
            return true;

        if (!subTranslatorAddr)
        {
            logger.warning("[nametable] sub_14076D950 not resolved — skipping");
            return false;
        }

        // Step 1: locate the `call sub_141D45270` inside sub_14076D950.
        // Historically at fixed offset +0x2A, but compiler prologue reshuffles
        // in future patches would silently drift that offset. Scan instead:
        // the `E8` call is preceded by a unique 14-byte anchor
        //   41 B8 01 00 00 00 48 8D 55 6F 48 8D 4D 87
        // (mov r8d,1 / lea rdx,[rbp+C6h] / lea rcx,[rbp+DEh]) which is
        // globally unique in v1.02.00 .text and stays stable as long as
        // the call site's register setup doesn't change. Bounded scan of
        // the first 0x80 bytes of the function so a false match elsewhere
        // can't leak in.
        const auto subTxStart = reinterpret_cast<const std::byte *>(subTranslatorAddr);
        auto anchor1 = DMK::Scanner::parse_aob(
            "41 B8 01 00 00 00 48 8D 55 6F 48 8D 4D 87 E8 | ?? ?? ?? ??");
        if (!anchor1)
        {
            logger.warning("[nametable] parse_aob failed for sub_141D45270 anchor");
            return false;
        }
        const auto *match1 = DMK::Scanner::find_pattern(subTxStart, 0x80, *anchor1);
        if (!match1)
        {
            logger.warning("[nametable] anchor for sub_141D45270 call "
                           "not found within sub_14076D950 prologue");
            return false;
        }
        // offset marker `|` points one past the E8 = start of disp32.
        const uintptr_t dispAddr1 = reinterpret_cast<uintptr_t>(match1) + anchor1->offset;
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

        // Step 3: locate the `mov rbx, cs:qword_145CEF370` inside
        // sub_1402D75D0. Historically at fixed offset +0x15. Scan instead:
        // the `48 8B 1D disp32` is preceded by a distinctive 6-byte
        // prologue-tail anchor
        //   41 56 48 83 EC 40 0F B7 39
        // (push r14 / sub rsp,40h / movzx edi,word ptr [rcx]) which
        // pins the specific call site inside a bounded 0x40-byte scan
        // of THIS function — global uniqueness doesn't matter, the
        // scan is locally bounded.
        const auto itemAccessorStart = reinterpret_cast<const std::byte *>(itemAccessor);
        auto anchor3 = DMK::Scanner::parse_aob(
            "41 56 48 83 EC 40 0F B7 39 | 48 8B 1D ?? ?? ?? ??");
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
        const uintptr_t ripInstr = reinterpret_cast<uintptr_t>(match3) + anchor3->offset;
        const auto disp = read_i32_safe(ripInstr + 3, ok);
        if (!ok)
        {
            logger.warning("[nametable] failed to read rip-disp at 0x{:X}",
                           ripInstr + 3);
            return false;
        }
        chain.globalHolder = (ripInstr + 7) + static_cast<intptr_t>(disp);
        chain.itemAccessor = itemAccessor;
        chain.resolved = true;
        logger.info(
            "[nametable] chain resolved: qword_145CEF370 holder = 0x{:X}, "
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
        info.ptrArray = read_qword_safe(
            globalPtr + k_iteminfoPtrArrayOffset, ok);
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
        const uintptr_t desc = read_qword_safe(
            ci.ptrArray + static_cast<uint64_t>(itemId) * 8, ok);
        return (ok && desc > 0x10000) ? desc : 0;
    }

    ItemNameTable::BuildResult ItemNameTable::build(uintptr_t subTranslatorAddr)
    {
        auto &logger = DMK::Logger::get_instance();

        // Step A: resolve and cache the address chain. Fatal on decoder
        // mismatch — retries won't help.
        if (!resolve_chain(subTranslatorAddr))
            return BuildResult::Fatal;

        const uintptr_t globalHolder = cached_chain().globalHolder;

        // Step B: dereference the holder. Deferred when null — the
        // game may not have initialized the iteminfo container yet.
        bool ok = false;
        const uintptr_t globalPtr = read_qword_safe(globalHolder, ok);
        if (!ok || globalPtr < 0x10000)
        {
            logger.trace("[nametable] iteminfo global not initialized "
                         "(holder=0x{:X} value=0x{:X}) — deferring",
                         globalHolder, globalPtr);
            return BuildResult::Deferred;
        }

        const uint32_t count = read_u32_safe(globalPtr + k_iteminfoCountOffset, ok);
        if (!ok || count == 0 || count > k_maxCatalogSize)
        {
            logger.trace("[nametable] catalog count implausible: {} "
                         "(globalPtr=0x{:X}) — deferring",
                         count, globalPtr);
            return BuildResult::Deferred;
        }

        const uintptr_t ptrArray = read_qword_safe(
            globalPtr + k_iteminfoPtrArrayOffset, ok);
        if (!ok || ptrArray < 0x10000)
        {
            logger.trace("[nametable] iteminfo ptrArray null "
                         "(globalPtr=0x{:X} ptrArray=0x{:X}) — deferring",
                         globalPtr, ptrArray);
            return BuildResult::Deferred;
        }

        logger.info("[nametable] scanning item catalog: count={} "
                    "globalPtr=0x{:X} ptrArray=0x{:X}",
                    count, globalPtr, ptrArray);

        const auto t0 = std::chrono::steady_clock::now();

        // Build into local maps first so the published snapshot is atomic
        // from any reader's viewpoint. Only copy into the member maps
        // under the mutex once walking is done.
        std::unordered_map<uint16_t, std::string> idToName;
        std::unordered_map<std::string, uint16_t> nameToId;
        std::unordered_map<uint16_t, uint8_t> variantFlag;
        std::unordered_map<uint16_t, uint8_t> playerFlag;
        idToName.reserve(count);
        nameToId.reserve(count);
        variantFlag.reserve(count);
        playerFlag.reserve(count);

        // Pass 1: walk the catalog, collect names + variant-meta pointers
        // + player-classifier flag for every valid descriptor. Variant flag
        // can't be resolved yet (sentinel is derived statistically in pass 2),
        // but the player-classifier check is self-contained per item.
        struct ScratchEntry
        {
            uint16_t id;
            std::string name;
            uintptr_t metaPtr;       // 0 on read fault
            bool playerCompatible;
        };
        std::vector<ScratchEntry> scratch;
        scratch.reserve(count);

        std::size_t valid = 0;
        std::size_t collisions = 0;
        std::size_t nonPlayerCount = 0;
        char buf[k_maxNameLen + 1];

        for (uint32_t id = 0; id < count; ++id)
        {
            const uintptr_t descPtr = read_qword_safe(ptrArray + id * 8ull, ok);
            if (!ok || descPtr < 0x10000)
                continue;

            const uintptr_t wrapper = read_qword_safe(descPtr + 8, ok);
            if (!ok || wrapper < 0x10000)
                continue;

            const uintptr_t strPtr = read_qword_safe(wrapper + 0, ok);
            if (!ok || strPtr < 0x10000)
                continue;

            const auto len = read_cstring_safe(strPtr, buf, sizeof(buf));
            if (len == 0 || len >= k_maxNameLen)
                continue;

            const auto id16 = static_cast<uint16_t>(id);
            const uintptr_t metaPtr = read_qword_safe(
                descPtr + k_descVariantMetaOffset, ok);
            const bool playerCompat = item_has_player_classifier(descPtr);
            if (!playerCompat)
                ++nonPlayerCount;
            scratch.push_back({
                id16,
                std::string(buf, len),
                ok ? metaPtr : 0,
                playerCompat,
            });
            ++valid;
        }

        // Pass 2 — statistically derive the variant-meta sentinel.
        // The sentinel is the value that appears at desc+0x3A0 in the
        // clear majority of items (historically ~60% on v1.02.00). Any
        // other pointer at that slot = per-item variant metadata and
        // gates out of runtime transmog.
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
            // Require the mode to dominate — at least 1/3 of valid items
            // must point at it — otherwise we're looking at garbage and
            // should not flag anything as variant.
            if (valid == 0 || sentinelCount * 3 < valid)
            {
                logger.debug("[nametable] variant sentinel not dominant "
                               "(best=0x{:X} count={}/{}) — disabling "
                               "variant-meta filter",
                               resolvedSentinel, sentinelCount, valid);
                resolvedSentinel = 0;
            }
            else
            {
                logger.info("[nametable] variant-meta sentinel resolved: "
                            "0x{:X} ({} of {} items)",
                            resolvedSentinel, sentinelCount, valid);
                s_variantMetaSentinel.store(
                    resolvedSentinel, std::memory_order_release);
            }
        }

        // Pass 3 — publish the scratch rows into the maps, flagging
        // variants against the resolved sentinel + the player-classifier
        // verdict captured in pass 1.
        std::size_t variantCount = 0;
        for (auto &e : scratch)
        {
            const bool hasVariant =
                (resolvedSentinel != 0) &&
                (e.metaPtr != 0) &&
                (e.metaPtr != resolvedSentinel);
            if (hasVariant)
                ++variantCount;

            idToName.emplace(e.id, e.name);
            auto [it, inserted] = nameToId.emplace(std::move(e.name), e.id);
            if (!inserted)
                ++collisions;
            variantFlag.emplace(e.id, hasVariant ? uint8_t{1} : uint8_t{0});
            playerFlag.emplace(e.id, e.playerCompatible ? uint8_t{1} : uint8_t{0});
        }

        // Stability check: the game sets the iteminfo count to its
        // final value (6024 on v1.02/v1.03) early but populates the
        // descriptor pointer array lazily.  Instead of comparing
        // against a magic "good enough" threshold (the old 50% gate
        // let a 3432/6024 partial catalog through on v1.03.01), we
        // wait until two consecutive scans produce the same valid
        // count — meaning the array has stopped growing.  This
        // self-adapts to any catalog size and any game version.
        if (valid == 0)
        {
            logger.trace("[nametable] no valid descriptors — deferring");
            m_lastBuildValid = 0;
            return BuildResult::Deferred;
        }
        if (valid != m_lastBuildValid)
        {
            logger.trace("[nametable] catalog still loading "
                         "({} -> {} valid) — deferring",
                         m_lastBuildValid, valid);
            m_lastBuildValid = static_cast<uint32_t>(valid);
            return BuildResult::Deferred;
        }
        // valid > 0 && valid == m_lastBuildValid → catalog stabilized.

        {
            std::lock_guard<std::mutex> lk(s_tableMtx);
            m_idToName = std::move(idToName);
            m_nameToId = std::move(nameToId);
            m_variantFlag = std::move(variantFlag);
            m_playerFlag = std::move(playerFlag);
            m_sortedCache.clear(); // will be rebuilt lazily on next access
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            t1 - t0)
                            .count();

        logger.info("[nametable] built: {}/{} entries ({} name collisions, "
                    "{} variant-meta, {} non-player) in {}ms",
                    valid, count, collisions, variantCount,
                    nonPlayerCount, ms);
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
        auto it = m_playerFlag.find(itemId);
        // Unknown ids default to true — prefer to surface rather than hide.
        if (it == m_playerFlag.end())
            return true;
        return it->second != 0;
    }

    const std::vector<ItemNameTable::Entry> &
    ItemNameTable::sorted_entries() const
    {
        std::lock_guard<std::mutex> lk(s_tableMtx);
        if (!m_sortedCache.empty() || m_idToName.empty())
            return m_sortedCache;

        m_sortedCache.reserve(m_idToName.size());
        for (const auto &[id, name] : m_idToName)
        {
            auto vit = m_variantFlag.find(id);
            const bool hasVariant = (vit != m_variantFlag.end()) && (vit->second != 0);
            auto pit = m_playerFlag.find(id);
            const bool isPlayer = (pit == m_playerFlag.end()) || (pit->second != 0);
            m_sortedCache.push_back({
                id,
                classify_slot(name),
                hasVariant,
                isPlayer,
                name,
            });
        }

        std::sort(m_sortedCache.begin(), m_sortedCache.end(),
                  [](const Entry &a, const Entry &b) {
                      // Case-insensitive lexicographic compare so "Kliff"
                      // and "kliff" sort together; items lowercase anyway
                      // but this survives future catalog changes.
                      const auto &sa = a.name;
                      const auto &sb = b.name;
                      const std::size_t n = (std::min)(sa.size(), sb.size());
                      for (std::size_t i = 0; i < n; ++i)
                      {
                          const auto ca = static_cast<unsigned char>(
                              std::tolower(static_cast<unsigned char>(sa[i])));
                          const auto cb = static_cast<unsigned char>(
                              std::tolower(static_cast<unsigned char>(sb[i])));
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

            out << "0x" << std::hex << std::uppercase << e.id << std::dec
                << '\t' << slotStr
                << '\t' << (e.hasVariantMeta ? "yes" : "no")
                << '\t' << (e.isPlayerCompatible ? "yes" : "no")
                << '\t' << e.name
                << '\n';
        }

        logger.info("[nametable] dumped {} entries to CrimsonDesertLiveTransmog_items.tsv",
                    entries.size());
    }

} // namespace Transmog
