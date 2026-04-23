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
#include <string_view>
#include <mutex>
#include <unordered_map>

namespace Transmog
{
    // Sensible upper bound on item descriptor catalog size.
    // v1.02.00 ships ~6024 entries; guard generously.
    static constexpr uint32_t k_maxCatalogSize = 0x20000;

    static constexpr std::size_t k_maxNameLen = 96;

    // Variant-metadata detection (see item_name_table.hpp::has_variant_meta).
    // Clean base items have `*(desc+<offset>) == <sentinel>` where
    // <sentinel> is a shared empty-object pointer (IDA: off_1459D0B38 on
    // v1.02.00 -- an IRefCounted vtable in the exe's .data section).
    // Non-sentinel values point to a per-item metadata struct threaded
    // through a catalog-wide linked list; members of this list failed to
    // render via runtime transmog on all tested samples.
    //
    // The field offset shifts across major game revisions as the
    // descriptor struct grows:
    //
    //   v1.02.00 -- +0x3A0 (original)
    //   v1.04.00 -- +0x3C8 (descriptor gained 0x28 bytes of new fields
    //               between +0x3A0 and +0x3C8; +0x3A0 now carries an
    //               unrelated `0x1FFFFFFFF` bit-pattern value)
    //
    // The sentinel value itself is also unstable (any .data reshuffle
    // moves it). Rather than hardcoding either the offset or the
    // sentinel RVA, the builder resolves the sentinel statistically at
    // catalog-build time: scan every valid descriptor's +0x3C8 qword,
    // the value that appears in the clear majority of items (~3-5k of
    // ~6k) IS the sentinel. Self-heals across future game updates as
    // long as the catalog stays statistically dominated by base items.
    static constexpr std::ptrdiff_t k_descVariantMetaOffset = 0x3C8;

    // --- iteminfo (qword_145CEF370) container layout, v1.02.00 ---
    // Source: IDA sub_1402D75D0 + static analysis. These are runtime data
    // offsets (not code) so they cannot be AOB-scanned; if a future patch
    // reshapes the struct the catalog walk will produce an implausible
    // count/ptrArray and bail at the existing sanity checks below.
    static constexpr std::ptrdiff_t k_iteminfoCountOffset = 0x08;    // dword entry count
    static constexpr std::ptrdiff_t k_iteminfoPtrArrayOffset = 0x50; // 80: qword base of descriptor ptr array

    // Resolved sentinel, cached after first successful build().
    // 0 means "not yet resolved" -- has_variant_meta() falls back to false
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

    static uint16_t read_u16_safe(uintptr_t addr, bool &ok) noexcept
    {
        __try
        {
            uint16_t v = *reinterpret_cast<const uint16_t *>(addr);
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
    //   - 731 items have no rules at all (default: treat as safe -- these
    //     are inert cosmetics / quest items, not mount armor)
    //   - 80/80 HorseArmor items correctly flagged unsafe; spot checks
    //     (Wolf_Pursuer, Bear_Chief, Kliff_Helm, Antra_Helm) all correct.
    //
    // If a future patch changes the player classifier set, re-run the
    // catalog differential against known-player vs known-mount items.
    static constexpr std::uint16_t k_playerClassifiers[] = {
        0x0015,
        0x0018,
        0x0058,
        0x02E3,
        0x039D,
    };
    // Body-type classifier token sets (CE-verified 2026-04-21). The
    // game's rule evaluator gates armor by body-type tokens in each
    // rule's classifier array. Humanoid party gear slots into two
    // body families:
    //
    //   Male:   {0x0018, 0x0058, 0x02E3}  -- Kliff / Oongka / most male NPCs
    //   Female: {0x0072, 0x0382, 0x0300}  -- Damiane / female NPCs
    //
    // Character-identity tokens (Kliff 0x0015/0x039D, Oongka 0x0028,
    // Demian 0x0021, Drottesel 0x012F, etc.) live alongside these and
    // let the engine gate per-character-specific variants. For the
    // picker filter, body-type membership is the decisive signal:
    // male-body items on a female skeleton produce broken meshes, and
    // vice versa.
    //
    // Items with NO body-type token (e.g. Drottesel_Leather_Armor has
    // only {0x012F}) are classified as Generic -- the engine accepts
    // them on any humanoid body, so the picker shows them for every
    // character.
    // Body-type classifier tokens on v1.04.00 (renumbered from v1.03.01
    // when the engine's class-enum was re-keyed; each token shifted by
    // +1 or +2). Verified live against the rule-classifier arrays of
    // WellsKnight_PlateArmor_{Helm, Armor} (known male carriers) and
    // Demian_Leather_Armor (known female carrier). Structure unchanged
    // at +0x248/+0x250 (rule list), +0x20/+0x28 (classifier list per
    // rule), 0x38 rule stride; only the enum values moved.
    //
    //   v1.03.01:  male   {0x0018, 0x0058, 0x02E3}
    //              female {0x0072, 0x0382, 0x0300}
    //   v1.04.00:  male   {0x0019, 0x0059, 0x02E5}
    //              female {0x0073, 0x0384, 0x0302}
    static constexpr std::uint16_t k_maleBodyTokens[] = {
        0x0019, 0x0059, 0x02E5,
    };
    static constexpr std::uint16_t k_femaleBodyTokens[] = {
        0x0073, 0x0384, 0x0302,
    };
    // Body bits are accumulated during the classifier scan:
    //   Male / Female  -- saw a token from the respective body set
    //   HasTokens      -- saw ANY classifier token (humanoid or not)
    //   NonHumanoid    -- saw a token >= 0x1000 (mount/pet/wagon/dragon
    //                     pools all sit in this range; every humanoid
    //                     token observed so far lives below 0x0400).
    // Classification (derived in sorted_entries):
    //   NonHumanoid set                      -> BodyKind::NonHumanoid (hide)
    //   Male+Female, no NonHumanoid          -> Both
    //   Male alone                           -> Male
    //   Female alone                         -> Female
    //   HasTokens but no body/NonHumanoid    -> Ambiguous  (e.g. {0x012F}-only
    //                                           NPC variants; render is
    //                                           inconsistent so the picker
    //                                           flags them with amber)
    //   No tokens at all                     -> Generic    (rule-less items)
    static constexpr std::uint8_t k_bodyBitMale = 0x01;
    static constexpr std::uint8_t k_bodyBitFemale = 0x02;
    static constexpr std::uint8_t k_bodyBitHasTokens = 0x04;
    static constexpr std::uint8_t k_bodyBitNonHumanoid = 0x08;
    // Set when the item has >= 2 rules that each contain a body token
    // (male or female) -- i.e. the item ships separate male and female
    // rule definitions, each gated on that rule's own identity tokens.
    // Empirically this correlates with "needs carrier to render on
    // the off-native-class character" (CE-verified 2026-04-21: Varantine,
    // Samuel, Heisellen all fit this pattern and need carrier for
    // Damiane; WellsKnight / Redknight have exactly one body-bearing
    // rule and direct-wear works for every protagonist).
    static constexpr std::uint8_t k_bodyBitMultiBodyRules = 0x10;
    // Any classifier token >= this threshold is treated as non-humanoid
    // (horse saddles, dragon armors, pet harnesses). CE-verified: every
    // humanoid token seen in v1.03.01 fits below 0x0400.
    static constexpr std::uint16_t k_nonHumanoidTokenThreshold = 0x1000;

    static constexpr std::ptrdiff_t k_ruleListPtrOffset = 0x248;
    static constexpr std::ptrdiff_t k_ruleListCountOffset = 0x250;
    static constexpr std::size_t k_ruleStride = 0x38;
    static constexpr std::ptrdiff_t k_ruleClassPtrOffset = 0x20;
    static constexpr std::ptrdiff_t k_ruleClassCountOffset = 0x28;
    static constexpr std::uint32_t k_maxPlausibleRuleCount = 64;
    static constexpr std::uint32_t k_maxPlausibleClassCount = 32;

    // Walk an item's rule-classifier arrays once and return a 2-bit
    // body-kind summary:
    //   bit 0 (k_bodyBitMale)   set if any rule has a male-body token
    //   bit 1 (k_bodyBitFemale) set if any rule has a female-body token
    // Items with no rules (or unreadable fields) return 0 (Generic) --
    // the picker treats Generic as "accepted by every body", matching
    // the engine's observed behaviour for rule-less cosmetics.
    static std::uint8_t item_body_bits(uintptr_t descPtr) noexcept
    {
        bool ok = false;
        const auto rulesPtr = read_qword_safe(
            descPtr + k_ruleListPtrOffset, ok);
        if (!ok || rulesPtr < 0x10000)
            return 0;
        const auto ruleCount = read_u32_safe(
            descPtr + k_ruleListCountOffset, ok);
        if (!ok || ruleCount == 0 || ruleCount > k_maxPlausibleRuleCount)
            return 0;

        std::uint8_t bits = 0;
        std::uint32_t bodyBearingRuleCount = 0; // rules with any body token

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

            bool ruleHasBodyToken = false;
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
                bits |= k_bodyBitHasTokens;
                if (cls >= k_nonHumanoidTokenThreshold)
                    bits |= k_bodyBitNonHumanoid;
                for (const auto mt : k_maleBodyTokens)
                {
                    if (cls == mt)
                    {
                        bits |= k_bodyBitMale;
                        ruleHasBodyToken = true;
                        break;
                    }
                }
                for (const auto ft : k_femaleBodyTokens)
                {
                    if (cls == ft)
                    {
                        bits |= k_bodyBitFemale;
                        ruleHasBodyToken = true;
                        break;
                    }
                }
            }
            if (ruleHasBodyToken)
                ++bodyBearingRuleCount;
        }

        // Dual-body NPC item: male and female variants split across
        // separate rules, each identity-gated. Render fidelity on an
        // off-native character requires the carrier mechanism.
        if (bodyBearingRuleCount >= 2)
            bits |= k_bodyBitMultiBodyRules;

        return bits;
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
                // Accept printable ASCII only -- reject if we see control
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
    //
    // Driven entirely by the canonical item-type code at desc+0x44 as
    // captured during the catalog build. Name-parsing heuristics have
    // been removed -- they produced false positives on non-armor items
    // whose names happened to contain tokens like "_Armor_" (horse
    // armors, shields, quest treasure maps, etc.). The game's own
    // type code is unambiguous; the fallback is to treat anything
    // unmapped as non-equipment.

    // Slot mapping for the canonical item-type code at desc+0x44.
    // Helm/Chest/Gloves/Boots verified on v1.03.01 (2026-04-21) and
    // confirmed unchanged on v1.04.00 (2026-04-23). Cloak shifted from
    // 0x45 -> 0x46 on v1.04.00 (one-code insertion in the engine enum
    // between Boots and Cloak). Both legacy and current values accepted
    // so the mapping stays valid across patches that haven't shifted
    // the earlier slots.
    //   0x04=Helm  0x05=Chest  0x06=Gloves  0x07=Boots
    //   0x45/0x46=Cloak
    // Other observed codes (explicitly rejected -- these are not
    // transmog-compatible armor slots):
    //   0x20=Shield, 0x53/0x54=Horse/mount armor, 0xFFFF=Quest/non-equipment,
    //   plus any other unmapped code.
    static TransmogSlot slot_from_type_code(std::uint16_t code) noexcept
    {
        switch (code)
        {
        case 0x04: return TransmogSlot::Helm;
        case 0x05: return TransmogSlot::Chest;
        case 0x06: return TransmogSlot::Gloves;
        case 0x07: return TransmogSlot::Boots;
        case 0x45:
        case 0x46: return TransmogSlot::Cloak;
        default:   return TransmogSlot::Count;
        }
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
        uintptr_t itemAccessor = 0; // sub_1402D75D0 -- IndexedStringA short->hash
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
            logger.warning("[nametable] sub_14076D950 not resolved -- skipping");
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

        // Step 3: locate the `mov rbx, cs:qword_145CEF370` inside
        // sub_1402D75D0. Historically at fixed offset +0x15. Scan instead:
        // the `48 8B 1D disp32` is preceded by a distinctive 6-byte
        // prologue-tail anchor
        //   41 56 48 83 EC 40 0F B7 39
        // (push r14 / sub rsp,40h / movzx edi,word ptr [rcx]) which
        // pins the specific call site inside a bounded 0x40-byte scan
        // of THIS function -- global uniqueness doesn't matter, the
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
        // DMK v3.0.2+ applies pattern.offset internally; do NOT add it again.
        const uintptr_t ripInstr = reinterpret_cast<uintptr_t>(match3);
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
        // mismatch -- retries won't help.
        if (!resolve_chain(subTranslatorAddr))
            return BuildResult::Fatal;

        const uintptr_t globalHolder = cached_chain().globalHolder;

        // Step B: dereference the holder. Deferred when null -- the
        // game may not have initialized the iteminfo container yet.
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

        const uintptr_t ptrArray = read_qword_safe(
            globalPtr + k_iteminfoPtrArrayOffset, ok);
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

        // Build into local maps first so the published snapshot is atomic
        // from any reader's viewpoint. Only copy into the member maps
        // under the mutex once walking is done.
        std::unordered_map<uint16_t, std::string> idToName;
        std::unordered_map<std::string, uint16_t> nameToId;
        std::unordered_map<uint16_t, uint8_t> variantFlag;
        std::unordered_map<uint16_t, uint8_t> playerFlag;
        std::unordered_map<uint16_t, uint16_t> equipTypeMap;
        std::unordered_map<uint16_t, uint8_t> bodyBitsMap;
        std::unordered_map<uint16_t, uint16_t> typeCodeMap;
        idToName.reserve(count);
        nameToId.reserve(count);
        variantFlag.reserve(count);
        playerFlag.reserve(count);
        equipTypeMap.reserve(count);
        bodyBitsMap.reserve(count);
        typeCodeMap.reserve(count);

        // Pass 1: walk the catalog, collect names + variant-meta pointers
        // + player-classifier flag for every valid descriptor. Variant flag
        // can't be resolved yet (sentinel is derived statistically in pass 2),
        // but the player-classifier check is self-contained per item.
        struct ScratchEntry
        {
            uint16_t id;
            std::string name;
            uintptr_t metaPtr; // 0 on read fault
            bool playerCompatible;
            uint16_t equipType; // raw u16 at desc+0x42, 0 on read fault
            uint8_t bodyBits; // k_bodyBitMale | k_bodyBitFemale
            uint16_t typeCode; // desc+0x44 -- canonical item-type code
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

            // Body-bits summary walked once; Kliff "PlayerSafe" is
            // equivalent to "item has a male-body token".
            const uint8_t bodyBits = item_body_bits(descPtr);
            const bool playerCompat = (bodyBits & k_bodyBitMale) != 0;
            if (!playerCompat)
                ++nonPlayerCount;

            bool etOk = false;
            const uint16_t equipType =
                read_u16_safe(descPtr + 0x42, etOk);

            // Item-type code at desc+0x44 (u16). CE-verified on
            // v1.03.01 (2026-04-21) and v1.04.00 (2026-04-23):
            //   0x04=Helm, 0x05=Chest, 0x06=Gloves, 0x07=Boots,
            //   0x45/0x46=Cloak (shifted +1 on v1.04.00),
            //   0x20=Shield, 0x53/0x54=Horse/mount armor,
            //   0xFFFF=Quest/Non-equipment.
            // This is the canonical game-side classifier for item category;
            // slot derivation no longer depends on parsing the item name.
            bool tcOk = false;
            const uint16_t typeCode =
                read_u16_safe(descPtr + 0x44, tcOk);

            scratch.push_back({
                id16,
                std::string(buf, len),
                ok ? metaPtr : 0,
                playerCompat,
                etOk ? equipType : uint16_t{0},
                bodyBits,
                tcOk ? typeCode : uint16_t{0xFFFF},
            });
            ++valid;
        }

        // Pass 2 -- statistically derive the variant-meta sentinel.
        // The sentinel is the value that appears at
        // desc+k_descVariantMetaOffset in the clear majority of items
        // (historically ~60% on v1.02.00). Any other pointer at that
        // slot = per-item variant metadata and gates out of runtime
        // transmog.
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
            // Require the mode to dominate -- at least 1/3 of valid items
            // must point at it -- otherwise we're looking at garbage and
            // should not flag anything as variant.
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
                s_variantMetaSentinel.store(
                    resolvedSentinel, std::memory_order_release);
            }
        }

        // Pass 3 -- publish the scratch rows into the maps, flagging
        // variants against the resolved sentinel + the player-classifier
        // verdict captured in pass 1.
        std::size_t variantCount = 0;
        for (auto &e : scratch)
        {
            // Item "has variant" (picker shows carrier-color) if either:
            //   - desc+k_descVariantMetaOffset is non-sentinel
            //     (traditional catalog-level variant-meta record), OR
            //   - the item has >= 2 body-bearing classifier rules,
            //     i.e. separate male + female identity-gated rules.
            //     Empirically these are dual-body NPC items that need
            //     the carrier mechanism to render on the off-native
            //     body (Varantine, Samuel, Heisellen pattern). An item
            //     with exactly one body-bearing rule (WellsKnight,
            //     Redknight) direct-wears because every protagonist's
            //     own class pool covers the identity tokens in that
            //     single rule.
            const bool hasVariantByMeta =
                (resolvedSentinel != 0) &&
                (e.metaPtr != 0) &&
                (e.metaPtr != resolvedSentinel);
            const bool hasMultiBodyRules =
                (e.bodyBits & k_bodyBitMultiBodyRules) != 0;
            const bool hasVariant = hasVariantByMeta || hasMultiBodyRules;
            if (hasVariant)
                ++variantCount;

            idToName.emplace(e.id, e.name);
            auto [it, inserted] = nameToId.emplace(std::move(e.name), e.id);
            if (!inserted)
                ++collisions;
            variantFlag.emplace(e.id, hasVariant ? uint8_t{1} : uint8_t{0});
            playerFlag.emplace(e.id, e.playerCompatible ? uint8_t{1} : uint8_t{0});
            if (e.equipType != 0)
                equipTypeMap.emplace(e.id, e.equipType);
            bodyBitsMap.emplace(e.id, e.bodyBits);
            typeCodeMap.emplace(e.id, e.typeCode);
        }

        // Stability check: the game sets the iteminfo count to its
        // final value (6024 on v1.02/v1.03) early but populates the
        // descriptor pointer array lazily.  Instead of comparing
        // against a magic "good enough" threshold (the old 50% gate
        // let a 3432/6024 partial catalog through on v1.03.01), we
        // wait until two consecutive scans produce the same valid
        // count -- meaning the array has stopped growing.  This
        // self-adapts to any catalog size and any game version.
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
        // valid > 0 && valid == m_lastBuildValid → catalog stabilized.

        {
            std::lock_guard<std::mutex> lk(s_tableMtx);
            m_idToName = std::move(idToName);
            m_nameToId = std::move(nameToId);
            m_variantFlag = std::move(variantFlag);
            m_playerFlag = std::move(playerFlag);
            m_equipType = std::move(equipTypeMap);
            m_bodyBits = std::move(bodyBitsMap);
            m_typeCode = std::move(typeCodeMap);
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
        // Unknown ids default to true -- prefer to surface rather than hide.
        if (it == m_playerFlag.end())
            return true;
        return it->second != 0;
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
        auto it = m_typeCode.find(itemId);
        if (it == m_typeCode.end())
            return TransmogSlot::Count;
        return slot_from_type_code(it->second);
    }

    ItemNameTable::BodyKind ItemNameTable::body_kind_for_character(
        const std::string &charName) noexcept
    {
        // Male: Kliff, Oongka (their native rule sets include male-body
        //       tokens {0x0018, 0x0058, 0x02E3}).
        // Female: Damiane (tokens {0x0072, 0x0382, 0x0300}).
        // Unknown characters default to Generic -- we don't know their
        // body, so treat every item as potentially compatible rather
        // than silently hiding their picker.
        if (charName == "Kliff" || charName == "Oongka")
            return BodyKind::Male;
        if (charName == "Damiane")
            return BodyKind::Female;
        return BodyKind::Generic;
    }

    bool ItemNameTable::has_npc_equip_type(uint16_t itemId) const noexcept
    {
        // Player (Kliff) equip-type u16 at desc+0x42 is 0x0004.
        // NPC items use 0x0001. Any value != 0x0004 needs the carrier
        // path so the pipeline sees a Kliff-compatible descriptor.
        static constexpr std::ptrdiff_t k_equipTypeOffset = 0x42;
        static constexpr std::uint16_t k_playerEquipType = 0x0004;

        const auto desc = descriptor_of(itemId);
        if (desc == 0)
            return false; // can't read -- default to direct apply

        __try
        {
            const auto eType =
                *reinterpret_cast<const volatile std::uint16_t *>(
                    desc + k_equipTypeOffset);
            return eType != k_playerEquipType;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
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
            auto eit = m_equipType.find(id);
            const uint16_t equipT = (eit != m_equipType.end()) ? eit->second : uint16_t{0};
            auto bit = m_bodyBits.find(id);
            // Unknown ids default to Generic (bodyBits=0). Picker
            // shows Generic on every character, matching the engine's
            // behaviour for rule-less cosmetics.
            const uint8_t bBits =
                (bit != m_bodyBits.end()) ? bit->second : uint8_t{0};
            BodyKind kind;
            const bool hasM = (bBits & k_bodyBitMale) != 0;
            const bool hasF = (bBits & k_bodyBitFemale) != 0;
            const bool hasT = (bBits & k_bodyBitHasTokens) != 0;
            const bool hasNH = (bBits & k_bodyBitNonHumanoid) != 0;
            // Order matters: an item that carries a male/female body
            // token is humanoid by definition, even if it ALSO carries
            // an NPC-class identity token in the 0x1xxx range (e.g.
            // Wellsknight, Blackstar, etc.). Checking the NonHumanoid
            // bit first would misclassify those as mount/pet armors
            // and flag them red for every character. NonHumanoid is
            // the fallback only when no body-token match is found.
            if (hasM && hasF)
                kind = BodyKind::Both;
            else if (hasM)
                kind = BodyKind::Male;
            else if (hasF)
                kind = BodyKind::Female;
            else if (hasNH)
                // Token >= 0x1000 with no humanoid body-token present
                // => actual mount/pet/wagon/dragon gear. Hide.
                kind = BodyKind::NonHumanoid;
            else if (hasT)
                // Humanoid-range tokens but no body-specific match --
                // NPC-variant glove/armor items that carry only
                // identity tokens like `0x012F`. Render behaviour is
                // inconsistent so the picker colours these amber.
                kind = BodyKind::Ambiguous;
            else
                kind = BodyKind::Generic;
            // Look up display name by lowercased internal name.
            std::string lowerName = name;
            for (auto &c : lowerName)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            auto dit = m_displayNames.find(lowerName);
            std::string dispName =
                (dit != m_displayNames.end()) ? dit->second : std::string();

            // Canonical item-type code at desc+0x44 is authoritative.
            // Unmapped codes (0x20 shield, 0x53 horse armor, 0xFFFF
            // quest, etc.) map to Count → item is hidden as non-
            // equipment. An absent type-code entry also collapses to
            // Count; there's no name-parsing fallback because the
            // engine itself reads this field to categorize the item.
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
                      // Sort by display name when available, else by
                      // internal name. Case-insensitive so "Kliff" and
                      // "kliff" sort together.
                      const auto &sa = a.displayName.empty()
                                           ? a.name
                                           : a.displayName;
                      const auto &sb = b.displayName.empty()
                                           ? b.name
                                           : b.displayName;
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

    void ItemNameTable::load_display_names(const std::string &tsvPath)
    {
        auto &logger = DMK::Logger::get_instance();

        std::ifstream file(tsvPath);
        if (!file.is_open())
        {
            logger.warning("[nametable] display names file not found: '{}'",
                           tsvPath);
            return;
        }

        std::unordered_map<std::string, std::string> names;
        names.reserve(6100);
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            if (line.back() == '\r')
                line.pop_back();

            const auto tab = line.find('\t');
            if (tab == std::string::npos || tab == 0 ||
                tab + 1 >= line.size())
                continue;

            std::string key = line.substr(0, tab);
            for (auto &c : key)
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            names.emplace(std::move(key), line.substr(tab + 1));
        }

        {
            std::lock_guard<std::mutex> lk(s_tableMtx);
            m_displayNames = std::move(names);
            // Callers must invoke load_display_names() before any
            // sorted_entries() access (i.e. before dump_catalog_tsv)
            // so the cache is still empty here -- no re-sort needed.
            m_sortedCache.clear();
        }

        logger.info("[nametable] loaded {} display names from '{}'",
                    m_displayNames.size(), tsvPath);
    }

    std::string ItemNameTable::display_name_of(
        std::string_view internalName) const
    {
        // Lowercase into a stack buffer to avoid heap allocation.
        // Item names in the catalog are bounded by k_maxNameLen (256).
        char buf[256];
        const auto len = (std::min)(internalName.size(), sizeof(buf) - 1);
        for (std::size_t i = 0; i < len; ++i)
            buf[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(internalName[i])));
        buf[len] = '\0';
        const std::string key{buf, len};

        std::lock_guard<std::mutex> lk(s_tableMtx);
        const auto it = m_displayNames.find(key);
        if (it == m_displayNames.end())
            return {};
        return it->second;
    }

} // namespace Transmog
