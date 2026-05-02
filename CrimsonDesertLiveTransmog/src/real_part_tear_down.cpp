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
        // sub_14075FE60 -- Safe scene-graph tear-down.
        //   Calls sub_1425EBAE0 internally. Does NOT mutate the
        //   authoritative equip table at *(a1+0x78).
        //
        // sub_1402D75D0 -- IndexedStringA short->hash lookup. Takes the
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
        // These are data-layout offsets, not AOB-anchored, so they
        // shift across game patches. Sanity checks (container >0x10000,
        // count <= k_maxPlausibleEntries, slotTag in plausible range)
        // bail out before touching anything dangerous if a future
        // patch reshapes the struct again.
        //
        // a1 is the SlotPopulator descriptor context. The container
        // pointer field shifted as the component grew new fields in
        // front of this slot:
        //   v1.03.01: container @ a1 + 0x78
        //   v1.04.00: container @ a1 + 0x88   (+0x10 of new fields)
        //   v1.05.00: container @ a1 + 0x88   (unchanged from v1.04)
        //
        // container layout (unchanged across all versions):
        //   +0x00 QWORD header (unused)
        //   +0x08 QWORD base address of entry array
        //   +0x10 DWORD live entry count
        //   +0x14 DWORD capacity
        //
        // entry layout shifted between v1.04 and v1.05:
        //   v1.04.00: stride 0xC8, slot tag @ +0xC0
        //   v1.05.00: stride 0xD0, slot tag @ +0xC8 (entry grew 8 bytes)
        //
        // entry fields (offsets within an entry, v1.05):
        //   +0x08 WORD  primary item word (0xFFFF or 0 == empty)
        //   +0x10 QWORD gate (must be non-zero for a live entry)
        //   +0xC8 WORD  slot tag (search key; helm=0x0003 .. cloak=0x0010)
        //
        // The alt item word at +0x88 used by older versions is no
        // longer relied on; the primary path at +0x08 is sufficient.
        // Slot-tag values themselves are unchanged across versions
        // (Helm=0x03, Chest=0x04, Gloves=0x05, Boots=0x06, Cloak=0x10);
        // only the position within the entry shifted.
        constexpr std::uintptr_t k_containerPtrOffset       = 0x88;
        constexpr std::uintptr_t k_containerArrayBaseOffset = 0x08;
        constexpr std::uintptr_t k_containerCountOffset     = 0x10;
        constexpr std::uintptr_t k_entryStride              = 0xD0;
        constexpr std::uintptr_t k_entryPrimaryWordOffset   = 0x08;
        constexpr std::uintptr_t k_entrySlotTagOffset       = 0xC8;
        constexpr std::uintptr_t k_entryGateOffset          = 0x10;

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

    bool is_actor_apply_ready(void *a1Raw) noexcept
    {
        const auto a1 = reinterpret_cast<std::uintptr_t>(a1Raw);
        if (a1 < 0x10000)
            return false;

        // Mirrors the preamble of `tear_down_real_part` exactly. Layout
        // constants are file-scope (not exported), so the probe lives in
        // this TU and is exposed via the namespace function only. SEH-
        // wrapped because the placeholder wrapper that the engine parks
        // user+0xD8 on during world load faults at the container deref --
        // that fault is the primary signal we are filtering on.
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
            if (arrayBase < 0x10000)
                return false;

            const auto count =
                *reinterpret_cast<volatile std::uint32_t *>(
                    container + k_containerCountOffset);

            // count is the slot-entry count, not the equipped-item count
            // (the iteration in tear_down_real_part visits all indices
            // 0..count and skips empty slots via primary==0xFFFF and
            // gate==0 inside the loop). A naked character still reports
            // a full slot count -- empty slots persist as 0xFFFF
            // sentinels. Therefore count==0 with a readable container
            // signals an actor whose container struct is allocated but
            // whose slot entries are not yet populated -- placeholder or
            // mid-wiring. The upper bound rejects torn reads that would
            // otherwise pass the lower bound by accident.
            return count >= 1 && count <= k_maxPlausibleEntries;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
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
                "range [{:#x}..{:#x}] -- rejecting",
                gameSlotTag, k_minPlausibleSlotTag, k_maxPlausibleSlotTag);
            return false;
        }

        std::uint32_t hash = 0;

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
                    "(arrayBase=0x{:X} count={}) -- layout may have shifted",
                    static_cast<std::uint64_t>(arrayBase), count);
                return false;
            }

            // First-entry sanity log, once per session.
            bool expected = false;
            if (g_loggedFirstEntry.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
            {
                const auto p0 =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        arrayBase + k_entryPrimaryWordOffset);
                const auto t0 =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        arrayBase + k_entrySlotTagOffset);
                const auto g0 =
                    *reinterpret_cast<volatile std::uint64_t *>(
                        arrayBase + k_entryGateOffset);
                logger.info(
                    "[dispatch] tear_down first-entry sanity: "
                    "count={} primary={:#06x} slotTag={:#06x}@+{:#x} "
                    "gate={:#018x}",
                    count, p0, t0,
                    static_cast<std::uint64_t>(k_entrySlotTagOffset),
                    static_cast<std::uint64_t>(g0));
            }

            std::uintptr_t foundEntry = 0;
            std::uint16_t  itemWord   = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto entry = arrayBase + k_entryStride * i;

                const auto primary =
                    *reinterpret_cast<volatile std::uint16_t *>(
                        entry + k_entryPrimaryWordOffset);
                if (primary == 0xFFFF || primary == 0)
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
                itemWord = primary;
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

            // Resolve hash via the engine's interner.
            std::uint16_t localWord = itemWord;
            void *hashPtr = lookupFn(&localWord);
            if (!hashPtr)
            {
                logger.trace(
                    "[dispatch] tear_down slot={:#06x} entryFound=true "
                    "primary={:#06x} hash=<lookup_null>",
                    gameSlotTag, itemWord);
                return false;
            }
            hash = *reinterpret_cast<volatile std::uint32_t *>(hashPtr);
            if (hash == 0 || hash == 0xFFFFFFFF)
            {
                logger.trace(
                    "[dispatch] tear_down slot={:#06x} entryFound=true "
                    "primary={:#06x} hash={:#010x} <rejected>",
                    gameSlotTag, itemWord, hash);
                return false;
            }

            // Safe scene-graph tear-down: routes through sub_1425EBAE0
            // and does NOT mutate the authoritative entry array we just
            // walked, which is what makes it safe to call mid-iteration.
            safeFn(static_cast<std::int64_t>(a1), hash,
                   static_cast<std::int16_t>(gameSlotTag));
            logger.trace(
                "[dispatch] tear_down slot={:#06x} entryFound=true "
                "primary={:#06x} hash={:#010x} result=true",
                gameSlotTag, itemWord, hash);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            logger.trace(
                "[dispatch] tear_down slot={:#06x} SEH caught fault",
                gameSlotTag);
            return false;
        }
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
                if (primary == 0xFFFF || primary == 0)
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

                return primary;
            }
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
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
