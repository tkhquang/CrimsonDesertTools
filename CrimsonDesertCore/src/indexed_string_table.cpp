#include <cdcore/indexed_string_table.hpp>

#include <DetourModKit.hpp>

#include <excpt.h>

#include <chrono>
#include <cstring>

namespace CDCore
{
    static constexpr std::size_t k_maxStringLen = 64;

    /** @brief Lowest address a populated slot's string pointer can hold; below it the slot is null or unpublished. */
    static constexpr std::uintptr_t k_minStringPtr = 0x10000;

    /** @brief Byte stride between consecutive table entries. */
    static constexpr std::uintptr_t k_entryStride = 16;

    /**
     * @brief Copy entry[hash]'s string into @p buf, but only when it starts with @p prefix.
     *
     * @details The prefix test runs before the copy on purpose. The table holds the engine's entire interned-string
     *          set, tens of thousands of entries, and only a few hundred carry the caller's prefix. Copying first and
     *          filtering afterwards would spend a full string read on entries that were never candidates; comparing
     *          the leading bytes first keeps a rejected entry down to the one cache line its string starts on.
     *
     * @return Copied length, or 0 for an empty slot, a prefix mismatch, or a faulting read. Callers cannot
     *         distinguish the three and do not need to.
     */
    static std::size_t read_table_entry(std::uintptr_t tableArray, std::uint32_t hash, const char *prefix,
                                        std::size_t prefixLen, char *buf, std::size_t bufSize) noexcept
    {
        __try
        {
            const auto entryAddr = tableArray + static_cast<std::uintptr_t>(hash) * k_entryStride;
            const auto strPtr = *reinterpret_cast<const std::uintptr_t *>(entryAddr);
            if (strPtr < k_minStringPtr)
                return 0;

            const auto *src = reinterpret_cast<const char *>(strPtr);

            // Testing the terminator alongside the mismatch is what stops the compare walking past the end of a
            // string shorter than the prefix. An empty prefix skips the loop, so every entry is a candidate.
            for (std::size_t i = 0; i < prefixLen; ++i)
            {
                const char c = src[i];
                if (c == '\0' || c != prefix[i])
                    return 0;
            }

            std::size_t len = 0;
            while (len < bufSize - 1 && src[len] != '\0')
            {
                buf[len] = src[len];
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

    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(std::uintptr_t mapLookupFunc,
                                                                             const IndexedStringScanConfig &cfg)
    {
        auto &logger = DMK::Logger::get_instance();
        std::unordered_map<std::string, std::uint32_t> nameToHash;

        if (!mapLookupFunc)
            return nameToHash;

        // Locate `48 8B 05 <disp32>` inside the first 0x40 bytes of mapLookupFunc. Bounded scan -- global uniqueness
        // irrelevant.
        const auto funcStart = reinterpret_cast<const std::byte *>(mapLookupFunc);
        auto ripAob = DMK::Scanner::parse_aob("48 8B 05 ?? ?? ?? ??");
        if (!ripAob)
        {
            logger.warning("{}: parse_aob failed for mapLookup rip-anchor", cfg.logLabel);
            return nameToHash;
        }
        const auto *ripMatch = DMK::Scanner::find_pattern(funcStart, 0x40, *ripAob);
        if (!ripMatch)
        {
            logger.warning("{}: `48 8B 05` rip-instruction not found in first 0x40 "
                           "bytes of mapLookupFunc (0x{:X})",
                           cfg.logLabel, mapLookupFunc);
            return nameToHash;
        }
        const auto ripInstr = reinterpret_cast<std::uintptr_t>(ripMatch);

        std::int32_t disp = 0;
        std::memcpy(&disp, reinterpret_cast<const void *>(ripInstr + 3), sizeof(std::int32_t));
        const auto instrEnd = ripInstr + 7;
        const auto globalPtrAddr = static_cast<std::uintptr_t>(static_cast<std::int64_t>(instrEnd) + disp);

        // globalPtrAddr is a RIP-resolved module slot; guard the read so a build whose layout shifted it outside
        // committed memory yields 0 (handled as "not yet initialized") rather than faulting.
        const auto globalPtr = DMKMemory::seh_read<std::uintptr_t>(globalPtrAddr).value_or(0);
        if (globalPtr < k_minStringPtr)
        {
            logger.trace("{}: global pointer not yet initialized (0x{:X})", cfg.logLabel, globalPtr);
            return nameToHash;
        }

        // globalPtr is a live game heap pointer that can tear or relocate across a world reload; a faulting read yields
        // 0 and routes to the "offset moved" warning below instead of crashing the caller.
        const auto tableArray = DMKMemory::seh_read<std::uintptr_t>(globalPtr + cfg.tableArrayOffset).value_or(0);
        if (tableArray < k_minStringPtr)
        {
            logger.warning("{}: tableArray is null/invalid (0x{:X}) -- offset 0x{:X} "
                           "inside globalPtr may have moved",
                           cfg.logLabel, tableArray, static_cast<std::int64_t>(cfg.tableArrayOffset));
            return nameToHash;
        }

        logger.trace("{}: globalPtr=0x{:X} tableArray=0x{:X} range=0x{:X}-0x{:X}", cfg.logLabel, globalPtr, tableArray,
                     cfg.tableScanMin, cfg.tableScanMax);

        const char *prefix = cfg.prefix ? cfg.prefix : "";
        const std::size_t prefixLen = std::strlen(prefix);

        const auto t0 = std::chrono::steady_clock::now();
        std::uint32_t entries = 0;
        char buf[k_maxStringLen + 1];

        for (std::uint32_t hash = cfg.tableScanMin; hash <= cfg.tableScanMax; ++hash)
        {
            const auto len = read_table_entry(tableArray, hash, prefix, prefixLen, buf, sizeof(buf));
            if (len == 0 || len >= k_maxStringLen)
                continue;

            nameToHash[std::string(buf, len)] = hash;
            ++entries;
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (entries == 0)
        {
            logger.warning("{}: 0 entries matching prefix '{}' found in range "
                           "0x{:X}..0x{:X} -- table not yet populated or prefix "
                           "missing from this build; deferring feature",
                           cfg.logLabel, prefix, cfg.tableScanMin, cfg.tableScanMax);
        }
        else
        {
            // Per-scan summary stays at TRACE so the deferred-scan poll path (called every 2s until table stability)
            // does not flood the INFO stream. Call sites that want a one-shot INFO line emit their own at init-time
            // decision points.
            logger.trace("{}: {} entries for prefix '{}' in range 0x{:X}..0x{:X} "
                         "in {}ms",
                         cfg.logLabel, entries, prefix, cfg.tableScanMin, cfg.tableScanMax, ms);
        }

        return nameToHash;
    }

} // namespace CDCore
