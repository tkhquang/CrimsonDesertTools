#include "aob_resolver.hpp"
#include "real_part_tear_down.hpp"
#include "shared_state.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace Transmog::RealPartTearDown
{
    namespace
    {
        // Helper function pointer types. Addresses come from
        // Transmog::resolved_addrs() (populated elsewhere in init()):
        //
        // sub_14075FE60 — Safe scene-graph tear-down.
        //   Calls sub_1425EBAE0 internally. Does NOT mutate the
        //   authoritative equip table at *(a1+0x78).
        //
        // sub_1402D75D0 — IndexedStringA short->hash lookup. Takes the
        //   address of a uint16_t slot id; returns a pointer whose
        //   first DWORD is the descriptor hash used by the rest of the
        //   equip pipeline.

        using SafeTearDownFn =
            std::int64_t(__fastcall *)(std::int64_t a1,
                                       std::uint32_t hash,
                                       std::int16_t slotTag);

        using IndexedStringLookupFn =
            void *(__fastcall *)(const std::uint16_t *slotIdPtr);

        std::atomic<SafeTearDownFn> g_safeTearDown{nullptr};
        std::atomic<IndexedStringLookupFn> g_indexedStringLookup{nullptr};
        std::atomic<bool> g_ready{false};

        // One-shot sanity log of the first PartDef entry so a future
        // patch reshaping the struct is immediately visible in the log
        // (the container ptr + entry offsets are not AOB-anchored).
        std::atomic<bool> g_loggedFirstEntry{false};

        // ---- Runtime struct layout for the PartDef/auth-table container ----
        //
        // Source of truth: IDA decompile of sub_148EB6700 +
        // cross-referenced via live CE dumps at multiple call sites on
        // v1.02.00. These are data-layout offsets so they cannot be
        // AOB-scanned. If a future patch reshapes the struct the sanity
        // checks below bail out (count > k_maxPlausibleEntries,
        // arrayBase < 0x10000, slotTag outside plausible range) before
        // we touch anything dangerous.
        //
        // a1 (SlotPopulator descriptor context) layout:
        //   +0x78 QWORD pointer to the PartDef/auth-table container.
        //
        // container layout:
        //   +0x00 QWORD header (unused by this module)
        //   +0x08 QWORD base address of stride-0xC8 entry array
        //   +0x10 DWORD live entry count
        //   +0x14 DWORD capacity
        //
        // entry layout (stride 0xC8 / 200 bytes):
        //   +0x08 WORD  primary item word (0xFFFF == empty slot)
        //   +0x10 QWORD gate (must be non-zero for live entries)
        //   +0x88 WORD  alt item word (0xFFFF → fall back to +0x08)
        //   +0xC0 WORD  slot tag (search key; helm=0x0003 .. boot=0x0010)
        constexpr std::uintptr_t k_containerPtrOffset      = 0x78;
        constexpr std::uintptr_t k_containerArrayBaseOffset = 0x08;
        constexpr std::uintptr_t k_containerCountOffset    = 0x10;
        constexpr std::uintptr_t k_entryStride             = 0xC8;
        constexpr std::uintptr_t k_entryPrimaryWordOffset  = 0x08;
        constexpr std::uintptr_t k_entryGateOffset         = 0x10;
        constexpr std::uintptr_t k_entryAltWordOffset      = 0x88;
        constexpr std::uintptr_t k_entrySlotTagOffset      = 0xC0;

        constexpr std::uint32_t k_maxPlausibleEntries      = 0x1000;
        constexpr std::uint16_t k_minPlausibleSlotTag      = 0x0003;
        constexpr std::uint16_t k_maxPlausibleSlotTag      = 0x0010;

        [[nodiscard]] bool plausible_slot_tag(std::uint16_t tag) noexcept
        {
            return tag >= k_minPlausibleSlotTag && tag <= k_maxPlausibleSlotTag;
        }

        // First-byte prologue sanity check. Thin wrapper that defers to
        // Transmog::sanity_check_function_prologue so every fn-ptr-store
        // site in the mod uses the same gate. Kept as a byte-buffer
        // overload for the log-then-gate pattern below where we already
        // captured 8 bytes for diagnostics.
        [[nodiscard]] bool looks_like_prologue(const std::uint8_t *p) noexcept
        {
            if (!p)
                return false;
            return Transmog::sanity_check_function_prologue(
                reinterpret_cast<uintptr_t>(p));
        }

        [[nodiscard]] bool safe_read_bytes(const void *addr,
                                           std::uint8_t *out,
                                           std::size_t n) noexcept
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
    }// namespace

    bool is_ready() noexcept
    {
        return g_ready.load(std::memory_order_acquire);
    }

    bool resolve_helpers() noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        const auto &addrs = resolved_addrs();
        const auto safeAddr = addrs.safeTearDown;
        const auto lookupAddr = addrs.indexedStringLookup;

        if (!safeAddr)
        {
            logger.warning(
                "[dispatch] tear_down: safeTearDown address not resolved "
                "(AOB scan failed in init)");
            return false;
        }
        if (!lookupAddr)
        {
            logger.warning(
                "[dispatch] tear_down: indexedStringLookup address not "
                "resolved (ItemNameTable chain walk has not run yet)");
            return false;
        }

        std::uint8_t safeBytes[8]{};
        std::uint8_t lookupBytes[8]{};

        if (!safe_read_bytes(reinterpret_cast<const void *>(safeAddr),
                             safeBytes, sizeof(safeBytes)))
        {
            logger.warning(
                "[dispatch] tear_down: cannot read safeTearDown@{:#x}",
                static_cast<std::uint64_t>(safeAddr));
            return false;
        }
        if (!safe_read_bytes(reinterpret_cast<const void *>(lookupAddr),
                             lookupBytes, sizeof(lookupBytes)))
        {
            logger.warning(
                "[dispatch] tear_down: cannot read indexedStringLookup@{:#x}",
                static_cast<std::uint64_t>(lookupAddr));
            return false;
        }

        auto fmt8 = [](const std::uint8_t *b) {
            char buf[32];
            std::snprintf(
                buf, sizeof(buf),
                "%02X %02X %02X %02X %02X %02X %02X %02X",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            return std::string(buf);
        };

        logger.info(
            "[dispatch] tear_down bytes: safe@{:#x}=[{}] lookup@{:#x}=[{}]",
            static_cast<std::uint64_t>(safeAddr), fmt8(safeBytes),
            static_cast<std::uint64_t>(lookupAddr), fmt8(lookupBytes));

        if (!looks_like_prologue(safeBytes))
        {
            logger.warning(
                "[dispatch] tear_down: safeTearDown prologue reject");
            return false;
        }
        if (!looks_like_prologue(lookupBytes))
        {
            logger.warning(
                "[dispatch] tear_down: indexedStringLookup prologue reject");
            return false;
        }

        g_safeTearDown.store(reinterpret_cast<SafeTearDownFn>(safeAddr),
                             std::memory_order_release);
        g_indexedStringLookup.store(
            reinterpret_cast<IndexedStringLookupFn>(lookupAddr),
            std::memory_order_release);
        g_ready.store(true, std::memory_order_release);

        logger.info(
            "[dispatch] tear_down helpers resolved: safe={:#x} lookup={:#x}",
            static_cast<std::uint64_t>(safeAddr),
            static_cast<std::uint64_t>(lookupAddr));
        return true;
    }

    bool tear_down_real_part(void *a1Raw, std::uint16_t gameSlotTag) noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        if (!g_ready.load(std::memory_order_acquire))
            return false;

        const auto safeFn = g_safeTearDown.load(std::memory_order_acquire);
        const auto lookupFn =
            g_indexedStringLookup.load(std::memory_order_acquire);
        if (!safeFn || !lookupFn)
            return false;

        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (a1 < 0x10000)
            return false;

        if (!plausible_slot_tag(gameSlotTag))
        {
            logger.warning(
                "[dispatch] tear_down: slot tag {:#06x} outside plausible "
                "range [{:#x}..{:#x}] — rejecting",
                gameSlotTag, k_minPlausibleSlotTag, k_maxPlausibleSlotTag);
            return false;
        }

        bool entryFound = false;
        std::uint32_t hash = 0;
        bool result = false;

        __try
        {
            const auto container =
                *reinterpret_cast<volatile std::uintptr_t *>(
                    a1 + k_containerPtrOffset);
            if (container < 0x10000)
                return false;

            const auto arrayBase =
                *reinterpret_cast<volatile std::uintptr_t *>(
                    container + k_containerArrayBaseOffset);
            const auto count =
                *reinterpret_cast<volatile std::uint32_t *>(
                    container + k_containerCountOffset);
            if (arrayBase < 0x10000 || count == 0 ||
                count > k_maxPlausibleEntries)
            {
                logger.warning(
                    "[dispatch] tear_down: container sanity failed "
                    "(arrayBase=0x{:X} count={}) — layout may have shifted",
                    static_cast<std::uint64_t>(arrayBase), count);
                return false;
            }

            // First-entry sanity log, once per session. Warns loudly if
            // the gate qword is zero — that would mean the struct
            // reshaped and every subsequent walk is reading garbage.
            bool expected = false;
            if (g_loggedFirstEntry.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
            {
                const auto firstEntry = arrayBase;
                const auto p0 =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        firstEntry + k_entryPrimaryWordOffset);
                const auto g0 =
                    *reinterpret_cast<volatile std::uint64_t *>(
                        firstEntry + k_entryGateOffset);
                const auto a0 =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        firstEntry + k_entryAltWordOffset);
                const auto t0 =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        firstEntry + k_entrySlotTagOffset);
                logger.info(
                    "[dispatch] tear_down first-entry sanity: "
                    "count={} primary={:#06x} gate={:#018x} alt={:#06x} "
                    "slotTag={:#06x}",
                    count, p0,
                    static_cast<std::uint64_t>(g0), a0, t0);
                if (g0 == 0)
                {
                    logger.warning(
                        "[dispatch] tear_down: first-entry gate is zero — "
                        "PartDef struct layout may have shifted, all "
                        "subsequent walks are suspect");
                }
            }

            std::uintptr_t foundEntry = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = arrayBase + k_entryStride * i;

                const auto primary =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        entry + k_entryPrimaryWordOffset);
                if (primary == 0xFFFF)
                    continue;

                const auto gate =
                    *reinterpret_cast<volatile std::uint64_t *>(
                        entry + k_entryGateOffset);
                if (gate == 0)
                    continue;

                const auto tag =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        entry + k_entrySlotTagOffset);
                if (tag != gameSlotTag)
                    continue;

                foundEntry = entry;
                break;
            }

            if (!foundEntry)
            {
                logger.trace(
                    "[dispatch] tear_down slot={:#06x} entryFound=false "
                    "(walked {} entries)",
                    gameSlotTag, count);
                return false;
            }

            entryFound = true;

            // Hash extraction (verbatim from sub_148EB6700 decompile).
            std::uint16_t itemWord =
                *reinterpret_cast<volatile std::uint16_t *>(
                    foundEntry + k_entryAltWordOffset);
            if (itemWord == 0xFFFF)
                itemWord = *reinterpret_cast<volatile std::uint16_t *>(
                    foundEntry + k_entryPrimaryWordOffset);

            // Copy to a stable local — the lookup fn takes the ADDRESS of a
            // short and may dereference it multiple times.
            std::uint16_t localWord = itemWord;
            void *hashPtr = lookupFn(&localWord);
            if (!hashPtr)
            {
                logger.trace(
                    "[dispatch] tear_down slot={:#06x} entryFound=true "
                    "itemWord={:#06x} hash=<lookup_null>",
                    gameSlotTag, itemWord);
                return false;
            }

            hash = *reinterpret_cast<volatile std::uint32_t *>(hashPtr);
            if (hash == 0 || hash == 0xFFFFFFFF)
            {
                logger.trace(
                    "[dispatch] tear_down slot={:#06x} entryFound=true "
                    "itemWord={:#06x} hash={:#010x} <rejected>",
                    gameSlotTag, itemWord, hash);
                return false;
            }

            // Safe scene-graph tear-down. This path goes through
            // sub_1425EBAE0 and does NOT touch the auth table.
            safeFn(static_cast<std::int64_t>(a1), hash,
                   static_cast<std::int16_t>(gameSlotTag));
            result = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.trace(
                "[dispatch] tear_down slot={:#06x} SEH caught fault",
                gameSlotTag);
            return false;
        }

        logger.trace(
            "[dispatch] tear_down slot={:#06x} entryFound={} hash={:#010x} result={}",
            gameSlotTag, entryFound, hash, result);
        return result;
    }

    std::uint16_t get_real_item_id(void *a1Raw,
                                   std::uint16_t gameSlotTag) noexcept
    {
        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (a1 < 0x10000)
            return 0;

        if (!plausible_slot_tag(gameSlotTag))
            return 0;

        __try
        {
            const auto container =
                *reinterpret_cast<volatile std::uintptr_t *>(
                    a1 + k_containerPtrOffset);
            if (container < 0x10000)
                return 0;

            const auto arrayBase =
                *reinterpret_cast<volatile std::uintptr_t *>(
                    container + k_containerArrayBaseOffset);
            const auto count =
                *reinterpret_cast<volatile std::uint32_t *>(
                    container + k_containerCountOffset);
            if (arrayBase < 0x10000 || count == 0 ||
                count > k_maxPlausibleEntries)
                return 0;

            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = arrayBase + k_entryStride * i;

                const auto primary =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        entry + k_entryPrimaryWordOffset);
                if (primary == 0xFFFF)
                    continue;

                const auto gate =
                    *reinterpret_cast<volatile std::uint64_t *>(
                        entry + k_entryGateOffset);
                if (gate == 0)
                    continue;

                const auto tag =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        entry + k_entrySlotTagOffset);
                if (tag != gameSlotTag)
                    continue;

                // Prefer +0x88 (alt id) when valid, fall back to +0x08
                // primary. Matches sub_148EB6700's word-selection recipe.
                std::uint16_t w =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        entry + k_entryAltWordOffset);
                if (w == 0xFFFF)
                    w = primary;
                return w;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return 0;
    }

    bool tear_down_by_item_id(void *a1Raw, std::uint16_t itemId,
                              std::uint16_t gameSlotTag) noexcept
    {
        auto &logger = DMK::Logger::get_instance();

        if (!g_ready.load(std::memory_order_acquire) || itemId == 0)
            return false;

        const auto safeFn = g_safeTearDown.load(std::memory_order_acquire);
        const auto lookupFn =
            g_indexedStringLookup.load(std::memory_order_acquire);
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
                logger.trace(
                    "[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} "
                    "hash=<lookup_null>",
                    gameSlotTag, itemId);
                return false;
            }

            hash = *reinterpret_cast<volatile std::uint32_t *>(hashPtr);
            if (hash == 0 || hash == 0xFFFFFFFF)
            {
                logger.trace(
                    "[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} "
                    "hash={:#010x} <rejected>",
                    gameSlotTag, itemId, hash);
                return false;
            }

            safeFn(static_cast<std::int64_t>(a1), hash,
                   static_cast<std::int16_t>(gameSlotTag));
            result = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.trace(
                "[dispatch] tear_down_fake slot={:#06x} SEH caught fault",
                gameSlotTag);
            return false;
        }

        logger.trace(
            "[dispatch] tear_down_fake slot={:#06x} itemId={:#06x} "
            "hash={:#010x} result={}",
            gameSlotTag, itemId, hash, result);
        return result;
    }
}// namespace Transmog::RealPartTearDown
