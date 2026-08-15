#include "aob_resolver.hpp"
#include "item_name_table.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"
#include "transmog_map.hpp"

#include <cdcore/controlled_char.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace Transmog::RealPartTearDown
{
    namespace
    {
        // Helper function pointer types. Addresses come from Transmog::resolved_addrs() (populated elsewhere in
        // init()):
        //
        // SafeTearDown -- engine scene-graph tear-down. It calls an internal scene-detach primitive. It does NOT
        //   mutate the authoritative equip table at a1+k_containerPtrOffset. The function is AOB-resolved, so a
        //   reader who cross-checks against a disassembler database must match the byte pattern in
        //   k_safeTearDownCandidates, never a name.
        //
        // IndexedStringLookup -- short->hash lookup. It takes the address of a uint16_t slot id. It returns a
        //   pointer whose first DWORD is the descriptor hash used by the rest of the equip pipeline.

        using SafeTearDownFn = std::int64_t(__fastcall *)(std::int64_t a1, std::uint32_t hash, std::int16_t slotTag);

        using IndexedStringLookupFn = void *(__fastcall *)(const std::uint16_t *slotIdPtr);

        std::atomic<SafeTearDownFn> g_safeTearDown{nullptr};
        std::atomic<IndexedStringLookupFn> g_indexedStringLookup{nullptr};
        std::atomic<bool> g_ready{false};

        // One-shot sanity log of the first PartDef entry so a future patch reshaping the struct is immediately visible
        // in the log (the container ptr + entry offsets are not AOB-anchored).
        std::atomic<bool> g_loggedFirstEntry{false};

        // Auth-table verbose dump tracker. It fires the full enumeration once per distinct (a1, count) pair, so
        // character switches and table resizes re-emit the slot inventory. A session of stable equip state does not
        // spam the log on every tear-down call. The dump answers the slot-discovery question: does a character hold
        // live entries for tags beyond the documented Helm/Chest/Gloves/Boots/Cloak (0x03/0x04/0x05/0x06/0x10) set?
        // The dump function itself is defined below the layout constants because it reads them.
        std::atomic<std::uintptr_t> g_lastDumpedA1{0};
        std::atomic<std::uint32_t> g_lastDumpedCount{0};
        // Hash over the entry-array's primary IDs so the dedupe also catches gear changes within the same (a1, count).
        // The auth table's `count` is the slot-array capacity, not the equipped count. Empty slots persist as 0xFFFF
        // sentinels, so equip and unequip never bump `count`. Without the content hash, the dedupe key never re-fires
        // after the first dump per actor.
        std::atomic<std::uint64_t> g_lastDumpedContentHash{0};

        // ---- Runtime struct layout for the PartDef/auth-table container ----
        //
        // These are data-layout offsets, not AOB-anchored, so they shift across game patches. Sanity checks (container
        // >0x10000, count <= k_maxPlausibleEntries, slotTag in plausible range) bail out before touching anything
        // dangerous if a future patch reshapes the struct again.
        //
        // a1 is the SlotPopulator descriptor context. The container pointer field moves whenever
        // pa::ClientEquipSlotActorComponent grows or shrinks fields in front of this slot. The delta is per-field. One
        // patch can shrink this component by 8 bytes and at the same time push unrelated descriptor fields the other
        // way, so never assume that one measured delta applies struct-wide.
        //
        // container layout:
        //   +0x00 QWORD header (unused)
        //   +0x08 QWORD base address of entry array
        //   +0x10 DWORD live entry count
        //   +0x14 DWORD capacity
        //
        // entry fields (offsets within an entry):
        //   +0x08 WORD  primary item word (0xFFFF or 0 == empty)
        //   +0x10 QWORD gate (must be non-zero for a live entry)
        //   +0xC8 WORD  slot tag (search key, helm=0x0003 .. cloak=0x0010)
        //
        // The alt item word at +0x88 used by older layouts is no longer read. The primary word at +0x08 is
        // sufficient. Slot-tag VALUES are stable across patches (Helm=0x03, Chest=0x04, Gloves=0x05, Boots=0x06,
        // Cloak=0x10). Only the position within the entry shifts.
        //
        // A stale container offset fails SILENTLY and disables the whole mod. The neighboring slot holds a packed
        // scalar, not a pointer. On most component instances that scalar is small, so the `< 0x10000` guard rejects
        // it, is_actor_apply_ready() returns false forever and the apply path reports zero applied slots. On other
        // instances the same scalar is large enough to PASS the guard, and the walk then reads garbage. Verify the
        // offset against live memory on patch day. The engine states it in its own auth-table walk, with the
        // component in the base register:
        //   `mov rax,[r13+0x80] ; mov rdx,[rax+08] ; mov eax,[rax+10]`
        // That is: container at +0x80, array base at +0x08, count at +0x10.
        constexpr std::uintptr_t k_containerPtrOffset = 0x80;
        constexpr std::uintptr_t k_containerArrayBaseOffset = 0x08;
        constexpr std::uintptr_t k_containerCountOffset = 0x10;

        // The entry stride and the slot-tag offset always move together by 8, so verify both together on patch day.
        // The engine's own BatchEquip auth-table search loop (see k_batchEquipCandidates) states both literally:
        //   `imul rcx,rax,0xD0 ; cmp [rdx+0xC8],r8w ; add rdx,0xD0`
        constexpr std::uintptr_t k_entryStride = 0xD0;
        constexpr std::uintptr_t k_entryPrimaryWordOffset = 0x08;
        constexpr std::uintptr_t k_entrySlotTagOffset = 0xC8;
        constexpr std::uintptr_t k_entryGateOffset = 0x10;

        constexpr std::uint32_t k_maxPlausibleEntries = 0x1000;
        // Slot-tag range covers the full engine taxonomy. This is a plausibility gate on a value read out of the auth
        // table, not a list of tags LT manages, so it stays inclusive of tags that map to no TransmogSlot. The upper
        // bound must cover the highest tag the engine emits, not the highest LT acts on: a tag above the bound is
        // rejected with a warning, so a bound left behind the engine turns a newly added slot into a confusing
        // rejection rather than a clean no-op. Re-read it from the auth-table dump when the engine gains a slot.
        constexpr std::uint16_t k_minPlausibleSlotTag = 0x0000;
        constexpr std::uint16_t k_maxPlausibleSlotTag = 0x0018;

        [[nodiscard]] bool plausible_slot_tag(std::uint16_t tag) noexcept
        {
            return tag >= k_minPlausibleSlotTag && tag <= k_maxPlausibleSlotTag;
        }

        // Engine-only slot tags absent from `TransmogSlot` (and therefore from `slot_metadata.hpp::k_slotMetadata`) but
        // still part of the engine taxonomy. Kept here so the dump emits a readable label instead of "?".
        // `is_documented_slot` deliberately returns false for these so the dump still flags them as NEW SLOT TAG
        // candidates the mod has not lifted into TransmogSlot yet.
        //
        // Only reached for a tag `slot_from_game_tag` does not resolve, so a tag lifted into TransmogSlot must be
        // dropped from this table: its case would be unreachable.
        [[nodiscard]] const char *engine_only_slot_name(std::uint16_t tag) noexcept
        {
            switch (tag)
            {
            case 0x0015:
                return "OongkaRocket";
            default:
                return "?";
            }
        }

        // Slot label resolver. It defers to `slot_metadata.hpp` (single source of truth for per-slot static data) when
        // the tag maps to a `TransmogSlot`. Otherwise it falls back to the engine-only override table for the tags LT
        // does not manage.
        [[nodiscard]] const char *known_slot_name(std::uint16_t tag) noexcept
        {
            if (const auto s = slot_from_game_tag(static_cast<std::int16_t>(tag)))
                return slot_meta(*s).displayName;
            return engine_only_slot_name(tag);
        }

        // A tag is "documented" iff it round-trips through `slot_metadata` (and therefore has a `TransmogSlot` enum
        // entry). The engine-only tag 0x0015 (OongkaRocket) lives outside the enum on purpose, so it falls through to
        // the false branch and gets flagged in the dump.
        [[nodiscard]] bool is_documented_slot(std::uint16_t tag) noexcept
        {
            return slot_from_game_tag(static_cast<std::int16_t>(tag)).has_value();
        }

        // Friendly name for the LT-internal TransmogSlot category that ItemNameTable::category_of returns when it
        // classifies an item by its descriptor (independent of the auth-table slot tag). It defers to slot_name(),
        // which knows every TransmogSlot enum entry. "Other" is the catch-all for items whose descriptor type-code did
        // not classify (it returned TransmogSlot::Count).
        [[nodiscard]] const char *transmog_category_str(TransmogSlot s) noexcept
        {
            if (s == TransmogSlot::Count)
                return "Other";
            return slot_name(s);
        }

        // Walks every live entry in the auth table once per (a1, count) change and logs (index, slotTag, primary,
        // gate-non-null) plus the resolved item name and LT-classified category. The caller's __try guards the reads.
        // The caller also validates container, arrayBase and count before the call. The cost is bounded: at most
        // k_maxPlausibleEntries iterations, and one log line per live entry. Item-name resolution falls back to
        // "<unresolved>" when ItemNameTable is not built yet (for example, early in load).
        void dump_full_auth_table_if_changed(std::uintptr_t a1, std::uintptr_t arrayBase, std::uint32_t count) noexcept
        {
            // Cheap pre-pass: FNV-1a 64-bit hash over the primary IDs so equip and unequip events, which leave count
            // unchanged, stay visible to the dedupe gate. Two bytes hashed per entry, so the cost is negligible. The
            // caller's __try frame covers the volatile reads.
            std::uint64_t contentHash = 0xCBF29CE484222325ULL;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = arrayBase + k_entryStride * i;
                const auto primary = *reinterpret_cast<volatile std::uint16_t *>(entry + k_entryPrimaryWordOffset);
                contentHash ^= static_cast<std::uint64_t>(primary);
                contentHash *= 0x100000001B3ULL;
            }

            const auto prevA1 = g_lastDumpedA1.load(std::memory_order_acquire);
            const auto prevCount = g_lastDumpedCount.load(std::memory_order_acquire);
            const auto prevHash = g_lastDumpedContentHash.load(std::memory_order_acquire);
            if (prevA1 == a1 && prevCount == count && prevHash == contentHash)
                return;

            // Stamp the dedupe tracker only when ItemNameTable is ready. Otherwise the first dump fires from the early
            // load-detect retry probe, which runs before the catalog is built. Names then show as "<unresolved>", and
            // a stamped tracker blocks every later dump. The deferred stamp lets the dump re-fire on each retry tick
            // through the load window. The cost is one log batch per tick, and the FINAL emission carries full item
            // names.
            auto &logger = DMK::Logger::get_instance();
            auto &itemTable = ItemNameTable::instance();
            const bool itemTableReady = itemTable.size() > 0;
            if (itemTableReady)
            {
                g_lastDumpedA1.store(a1, std::memory_order_release);
                g_lastDumpedCount.store(count, std::memory_order_release);
                g_lastDumpedContentHash.store(contentHash, std::memory_order_release);
            }

            logger.trace("[slot-discovery] auth-table dump begin a1=0x{:X} "
                         "arrayBase=0x{:X} count={} stride={:#x} slotTag@+{:#x} "
                         "ItemNameTable={}",
                         static_cast<std::uint64_t>(a1), static_cast<std::uint64_t>(arrayBase), count,
                         static_cast<std::uint64_t>(k_entryStride), static_cast<std::uint64_t>(k_entrySlotTagOffset),
                         itemTableReady ? "ready" : "not-ready");

            std::uint32_t live = 0;
            std::uint32_t documented = 0;
            std::uint32_t newTags = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = arrayBase + k_entryStride * i;
                const auto primary = *reinterpret_cast<volatile std::uint16_t *>(entry + k_entryPrimaryWordOffset);
                const auto gate = *reinterpret_cast<volatile std::uint64_t *>(entry + k_entryGateOffset);
                const auto tag = *reinterpret_cast<volatile std::uint16_t *>(entry + k_entrySlotTagOffset);

                const bool isLive = !(primary == 0xFFFF || primary == 0) && gate != 0;
                if (!isLive)
                    continue;

                ++live;
                const bool documentedTag = is_documented_slot(tag);
                if (documentedTag)
                    ++documented;
                else
                    ++newTags;

                const char *tagName = known_slot_name(tag);
                const char *newTagMarker = documentedTag ? "" : "  *** NEW SLOT TAG ***";

                if (!itemTableReady)
                {
                    // Catalog not built yet. Skip the resolved fields entirely, because they are all default-init
                    // noise (typeCode=0xFFFF, cat=Other), and print the raw engine-side fields only. This branch
                    // re-fires on the next probe tick after names land.
                    logger.trace("[slot-discovery]   [{:>2}] tag={:#06x} ({:<12}) "
                                 "primary={:#06x} <unresolved>{}",
                                 i, tag, tagName, primary, newTagMarker);
                    continue;
                }

                std::string itemName = itemTable.name_of(primary);
                if (itemName.empty())
                    itemName = "<unresolved>";

                const char *categoryStr = transmog_category_str(itemTable.category_of(primary));
                const std::uint16_t typeCode = itemTable.type_code_of(primary);

                // Auto-record (itemId -> TransmogSlot) for the picker catalog. The auth-table tag states which slot
                // this item belongs in. That is ground truth, and it overrides the static type-code heuristic. The
                // record is skipped when the tag has no TransmogSlot mapping (for example tag 0x15 OongkaRocket, which
                // is intentionally outside TransmogSlot).
                //
                // A DISABLED slot is skipped as well, because the binding is sticky for the session and its only
                // consumer is the picker, which never lists a disabled slot. The overflow slots make that concrete:
                // the engine parks a weapon there whenever its primary slot is taken (a shield lands in OffHand2 while
                // a dual-wielded sword holds OffHand, a bow lands in Ranged2 while a sprayer holds Ranged). Recording
                // that would rebind the item to a slot with no picker and drop it out of the primary slot's list for
                // the rest of the session, even after the engine moves it back. Falling through to the static
                // type-code map keeps such an item in the list it belongs to.
                if (auto tslot = slot_from_game_slot(static_cast<std::int16_t>(tag));
                    tslot.has_value() && Transmog::slot_enabled(*tslot))
                {
                    itemTable.record_observed_slot(primary, *tslot);
                }

                logger.trace("[slot-discovery]   [{:>2}] tag={:#06x} ({:<12}) "
                             "primary={:#06x} typeCode={:#06x} "
                             "cat={:<13} name=\"{}\"{}",
                             i, tag, tagName, primary, typeCode, categoryStr, itemName, newTagMarker);
            }

            logger.trace("[slot-discovery] auth-table dump end live={} "
                         "documented={} new_tags={} runtime_obs_total={} "
                         "(NEW SLOT TAG entries are candidates for TransmogSlot "
                         "enum extension; runtime_obs_total counts session-wide "
                         "(itemId->slot) bindings the picker now uses to override "
                         "static type-code mapping)",
                         live, documented, newTags, itemTableReady ? itemTable.observed_slot_count() : std::size_t{0});
        }

        // First-byte prologue sanity check. Thin wrapper that defers to DMK::Scanner::is_likely_function_prologue so
        // every fn-ptr-store site in the mod uses the same gate. Kept as a byte-buffer overload for the log-then-gate
        // pattern below where we already captured 8 bytes for diagnostics.
        [[nodiscard]] bool looks_like_prologue(const std::uint8_t *p) noexcept
        {
            if (!p)
                return false;
            return DMK::Scanner::is_likely_function_prologue(reinterpret_cast<uintptr_t>(p));
        }

        [[nodiscard]] bool safe_read_bytes(const void *addr, std::uint8_t *out, std::size_t n) noexcept
        {
            __try
            {
                std::memcpy(out, addr, n);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    } // namespace

    bool is_ready() noexcept
    {
        return g_ready.load(std::memory_order_acquire);
    }

    bool is_actor_apply_ready(void *a1Raw) noexcept
    {
        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (a1 < 0x10000)
            return false;

        // Stage 1: structural reads under SEH (POD locals only).
        std::uintptr_t arrayBase = 0;
        std::uint32_t count = 0;
        __try
        {
            const auto container = *reinterpret_cast<volatile std::uintptr_t *>(a1 + k_containerPtrOffset);
            if (container < 0x10000)
                return false;

            arrayBase = *reinterpret_cast<volatile std::uintptr_t *>(container + k_containerArrayBaseOffset);
            if (arrayBase < 0x10000)
                return false;

            count = *reinterpret_cast<volatile std::uint32_t *>(container + k_containerCountOffset);
            if (count < 1 || count > k_maxPlausibleEntries)
                return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        // Stage 2: engine readiness. SafeTearDown performs a deep dereference of the shape
        //     sub = *(CCC + 0x130);
        //     ... *sub ...
        // During cold-load the field at `CCC + 0x130` is null. After the engine wires up the actor's scene graph, the
        // field holds a heap-allocated sub-handler pointer. SafeTearDown's own null-check guards only against a null
        // CCC, not against this deeper field, so a call before the field is populated faults inside the engine. A
        // probe of this one pointer is necessary, because SafeTearDown faults when it is null. It is also sufficient,
        // because the transition coincides with the end of the cold-load fault window.
        //
        // The CCC instance is located by MSVC RTTI mangled name rather than by a fixed slot offset, so the slot drift
        // documented in CDCore controlled_char does not affect this probe. Only the +0x130 sub-handler offset lives in
        // LT. The a1 -> CCOIA -> p1 chain offsets live inside CDCore.
        constexpr std::ptrdiff_t k_offCccSubHandler = 0x130;
        static std::atomic<std::uintptr_t> s_cccVt{0};
        static constexpr std::string_view k_cccName = ".?AVClientCharacterControlActorComponent@pa@@";

        const auto cccAddr = CDCore::find_component_for_equipslot(a1, k_cccName, s_cccVt);
        if (cccAddr < 0x10000)
            return false;

        std::uintptr_t subHandler = 0;
        __try
        {
            subHandler = *reinterpret_cast<volatile std::uintptr_t *>(cccAddr + k_offCccSubHandler);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (subHandler < 0x10000)
            return false;

        dump_full_auth_table_if_changed(a1, arrayBase, count);
        return true;
    }

    bool resolve_helpers() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        const auto &addrs = resolved_addrs();
        const auto safeAddr = addrs.safeTearDown;
        const auto lookupAddr = addrs.indexedStringLookup;

        if (!safeAddr)
        {
            logger.warning("[dispatch] tear_down: safeTearDown address not resolved "
                           "(AOB scan failed in init)");
            return false;
        }
        if (!lookupAddr)
        {
            logger.warning("[dispatch] tear_down: indexedStringLookup address not "
                           "resolved (ItemNameTable chain walk has not run yet)");
            return false;
        }

        std::uint8_t safeBytes[8]{};
        std::uint8_t lookupBytes[8]{};

        if (!safe_read_bytes(reinterpret_cast<const void *>(safeAddr), safeBytes, sizeof(safeBytes)))
        {
            logger.warning("[dispatch] tear_down: cannot read safeTearDown@{:#x}",
                           static_cast<std::uint64_t>(safeAddr));
            return false;
        }
        if (!safe_read_bytes(reinterpret_cast<const void *>(lookupAddr), lookupBytes, sizeof(lookupBytes)))
        {
            logger.warning("[dispatch] tear_down: cannot read indexedStringLookup@{:#x}",
                           static_cast<std::uint64_t>(lookupAddr));
            return false;
        }

        auto fmt8 = [](const std::uint8_t *b)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%02X %02X %02X %02X %02X %02X %02X %02X", b[0], b[1], b[2], b[3], b[4],
                          b[5], b[6], b[7]);
            return std::string(buf);
        };

        logger.info("[dispatch] tear_down bytes: safe@{:#x}=[{}] lookup@{:#x}=[{}]",
                    static_cast<std::uint64_t>(safeAddr), fmt8(safeBytes), static_cast<std::uint64_t>(lookupAddr),
                    fmt8(lookupBytes));

        if (!looks_like_prologue(safeBytes))
        {
            logger.warning("[dispatch] tear_down: safeTearDown prologue reject");
            return false;
        }
        if (!looks_like_prologue(lookupBytes))
        {
            logger.warning("[dispatch] tear_down: indexedStringLookup prologue reject");
            return false;
        }

        g_safeTearDown.store(reinterpret_cast<SafeTearDownFn>(safeAddr), std::memory_order_release);
        g_indexedStringLookup.store(reinterpret_cast<IndexedStringLookupFn>(lookupAddr), std::memory_order_release);

        g_ready.store(true, std::memory_order_release);

        logger.info("[dispatch] tear_down helpers resolved: safe={:#x} lookup={:#x}",
                    static_cast<std::uint64_t>(safeAddr), static_cast<std::uint64_t>(lookupAddr));
        return true;
    }

    bool tear_down_real_part(void *a1Raw, std::uint16_t gameSlotTag) noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        if (!g_ready.load(std::memory_order_acquire))
            return false;

        const auto safeFn = g_safeTearDown.load(std::memory_order_acquire);
        const auto lookupFn = g_indexedStringLookup.load(std::memory_order_acquire);
        if (!safeFn || !lookupFn)
            return false;

        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (a1 < 0x10000)
            return false;

        if (!plausible_slot_tag(gameSlotTag))
        {
            logger.warning("[dispatch] tear_down: slot tag {:#06x} outside plausible "
                           "range [{:#x}..{:#x}] -- rejecting",
                           gameSlotTag, k_minPlausibleSlotTag, k_maxPlausibleSlotTag);
            return false;
        }

        std::uint32_t hash = 0;

        __try
        {
            const auto container = *reinterpret_cast<volatile std::uintptr_t *>(a1 + k_containerPtrOffset);
            if (container < 0x10000)
                return false;

            const auto arrayBase = *reinterpret_cast<volatile std::uintptr_t *>(container + k_containerArrayBaseOffset);
            const auto count = *reinterpret_cast<volatile std::uint32_t *>(container + k_containerCountOffset);
            if (arrayBase < 0x10000 || count == 0 || count > k_maxPlausibleEntries)
            {
                logger.warning("[dispatch] tear_down: container sanity failed "
                               "(arrayBase=0x{:X} count={}) -- layout may have shifted",
                               static_cast<std::uint64_t>(arrayBase), count);
                return false;
            }

            // First-entry sanity log, once per session.
            bool expected = false;
            if (g_loggedFirstEntry.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                const auto p0 = *reinterpret_cast<volatile std::uint16_t *>(arrayBase + k_entryPrimaryWordOffset);
                const auto t0 = *reinterpret_cast<volatile std::uint16_t *>(arrayBase + k_entrySlotTagOffset);
                const auto g0 = *reinterpret_cast<volatile std::uint64_t *>(arrayBase + k_entryGateOffset);
                logger.info("[dispatch] tear_down first-entry sanity: "
                            "count={} primary={:#06x} slotTag={:#06x}@+{:#x} "
                            "gate={:#018x}",
                            count, p0, t0, static_cast<std::uint64_t>(k_entrySlotTagOffset),
                            static_cast<std::uint64_t>(g0));
            }

            // Verbose slot-discovery dump: enumerates every live entry so additional slot tags (lower body, mask, neck,
            // etc.) populated by the engine for the active character become visible. One-shot per distinct (a1, count)
            // pair.
            dump_full_auth_table_if_changed(a1, arrayBase, count);

            std::uintptr_t foundEntry = 0;
            std::uint16_t itemWord = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = arrayBase + k_entryStride * i;

                const auto primary = *reinterpret_cast<volatile std::uint16_t *>(entry + k_entryPrimaryWordOffset);
                if (primary == 0xFFFF || primary == 0)
                    continue;

                const auto gate = *reinterpret_cast<volatile std::uint64_t *>(entry + k_entryGateOffset);
                if (gate == 0)
                    continue;

                const auto tag = *reinterpret_cast<volatile std::uint16_t *>(entry + k_entrySlotTagOffset);
                if (tag != gameSlotTag)
                    continue;

                foundEntry = entry;
                itemWord = primary;
                break;
            }

            if (!foundEntry)
            {
                logger.trace("[dispatch] tear_down slot={:#06x} entryFound=false "
                             "(walked {} entries)",
                             gameSlotTag, count);
                return false;
            }

            // Resolve hash via the engine's interner.
            std::uint16_t localWord = itemWord;
            void *hashPtr = lookupFn(&localWord);
            if (!hashPtr)
            {
                logger.trace("[dispatch] tear_down slot={:#06x} entryFound=true "
                             "primary={:#06x} hash=<lookup_null>",
                             gameSlotTag, itemWord);
                return false;
            }
            hash = *reinterpret_cast<volatile std::uint32_t *>(hashPtr);
            if (hash == 0 || hash == 0xFFFFFFFF)
            {
                logger.trace("[dispatch] tear_down slot={:#06x} entryFound=true "
                             "primary={:#06x} hash={:#010x} <rejected>",
                             gameSlotTag, itemWord, hash);
                return false;
            }

            // Safe scene-graph tear-down. It routes through the engine's scene-detach primitive and does NOT mutate
            // the authoritative entry array, so the apply dispatcher can call it.
            safeFn(static_cast<std::int64_t>(a1), hash, static_cast<std::int16_t>(gameSlotTag));
            logger.trace("[dispatch] tear_down slot={:#06x} entryFound=true "
                         "primary={:#06x} hash={:#010x} result=true",
                         gameSlotTag, itemWord, hash);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.trace("[dispatch] tear_down slot={:#06x} SEH caught fault", gameSlotTag);
            return false;
        }
    }

    std::uint16_t get_real_item_id(void *a1Raw, std::uint16_t gameSlotTag) noexcept
    {
        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (a1 < 0x10000)
            return 0;

        if (!plausible_slot_tag(gameSlotTag))
            return 0;

        __try
        {
            const auto container = *reinterpret_cast<volatile std::uintptr_t *>(a1 + k_containerPtrOffset);
            if (container < 0x10000)
                return 0;

            const auto arrayBase = *reinterpret_cast<volatile std::uintptr_t *>(container + k_containerArrayBaseOffset);
            const auto count = *reinterpret_cast<volatile std::uint32_t *>(container + k_containerCountOffset);
            if (arrayBase < 0x10000 || count == 0 || count > k_maxPlausibleEntries)
                return 0;

            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = arrayBase + k_entryStride * i;

                const auto primary = *reinterpret_cast<volatile std::uint16_t *>(entry + k_entryPrimaryWordOffset);
                if (primary == 0xFFFF || primary == 0)
                    continue;

                const auto gate = *reinterpret_cast<volatile std::uint64_t *>(entry + k_entryGateOffset);
                if (gate == 0)
                    continue;

                const auto tag = *reinterpret_cast<volatile std::uint16_t *>(entry + k_entrySlotTagOffset);
                if (tag != gameSlotTag)
                    continue;

                return primary;
            }
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool tear_down_by_item_id(void *a1Raw, std::uint16_t itemId, std::uint16_t gameSlotTag) noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        if (!g_ready.load(std::memory_order_acquire) || itemId == 0)
            return false;

        const auto safeFn = g_safeTearDown.load(std::memory_order_acquire);
        const auto lookupFn = g_indexedStringLookup.load(std::memory_order_acquire);
        if (!safeFn || !lookupFn)
            return false;

        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (a1 < 0x10000)
            return false;

        std::uint32_t hash = 0;
        bool result = false;

        __try
        {
            std::uint16_t localWord = itemId;
            void *hashPtr = lookupFn(&localWord);
            if (!hashPtr)
            {
                logger.trace("[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} "
                             "hash=<lookup_null>",
                             gameSlotTag, itemId);
                return false;
            }

            hash = *reinterpret_cast<volatile std::uint32_t *>(hashPtr);
            if (hash == 0 || hash == 0xFFFFFFFF)
            {
                logger.trace("[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} "
                             "hash={:#010x} <rejected>",
                             gameSlotTag, itemId, hash);
                return false;
            }

            safeFn(static_cast<std::int64_t>(a1), hash, static_cast<std::int16_t>(gameSlotTag));
            result = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.trace("[dispatch] tear_down_fake slot={:#06x} SEH caught fault", gameSlotTag);
            return false;
        }

        logger.trace("[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} "
                     "hash={:#010x} result={}",
                     gameSlotTag, itemId, hash, result);
        return result;
    }
} // namespace Transmog::RealPartTearDown
