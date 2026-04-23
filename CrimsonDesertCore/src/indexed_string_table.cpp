#include <cdcore/indexed_string_table.hpp>

#include <DetourModKit.hpp>

#include <Windows.h>

#include <chrono>
#include <cstring>

namespace CDCore
{
    static constexpr std::size_t k_maxStringLen = 64;

    static std::size_t read_table_entry(
        std::uintptr_t tableArray, std::uint32_t hash,
        char *buf, std::size_t bufSize) noexcept
    {
        __try
        {
            const auto entryAddr =
                tableArray + static_cast<std::uintptr_t>(hash) * 16;
            const auto strPtr =
                *reinterpret_cast<const std::uintptr_t *>(entryAddr);
            if (strPtr < 0x10000)
                return 0;

            const auto *src = reinterpret_cast<const char *>(strPtr);
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

    static bool matches_prefix(
        const char *buf, std::size_t len, const char *prefix) noexcept
    {
        if (!prefix || !*prefix)
            return true;
        const std::size_t plen = std::strlen(prefix);
        if (len < plen)
            return false;
        return std::memcmp(buf, prefix, plen) == 0;
    }

    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(
        std::uintptr_t mapLookupFunc,
        const IndexedStringScanConfig &cfg)
    {
        auto &logger = DMK::Logger::get_instance();
        std::unordered_map<std::string, std::uint32_t> nameToHash;

        if (!mapLookupFunc)
            return nameToHash;

        // Locate `48 8B 05 <disp32>` inside the first 0x40 bytes of
        // mapLookupFunc. Bounded scan -- global uniqueness irrelevant.
        const auto funcStart =
            reinterpret_cast<const std::byte *>(mapLookupFunc);
        auto ripAob = DMK::Scanner::parse_aob("48 8B 05 ?? ?? ?? ??");
        if (!ripAob)
        {
            logger.warning(
                "{}: parse_aob failed for mapLookup rip-anchor",
                cfg.logLabel);
            return nameToHash;
        }
        const auto *ripMatch =
            DMK::Scanner::find_pattern(funcStart, 0x40, *ripAob);
        if (!ripMatch)
        {
            logger.warning(
                "{}: `48 8B 05` rip-instruction not found in first 0x40 "
                "bytes of mapLookupFunc (0x{:X})",
                cfg.logLabel, mapLookupFunc);
            return nameToHash;
        }
        const auto ripInstr = reinterpret_cast<std::uintptr_t>(ripMatch);

        std::int32_t disp = 0;
        std::memcpy(&disp, reinterpret_cast<const void *>(ripInstr + 3),
                    sizeof(std::int32_t));
        const auto instrEnd = ripInstr + 7;
        const auto globalPtrAddr = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(instrEnd) + disp);

        const auto globalPtr =
            *reinterpret_cast<const std::uintptr_t *>(globalPtrAddr);
        if (globalPtr < 0x10000)
        {
            logger.trace(
                "{}: global pointer not yet initialized (0x{:X})",
                cfg.logLabel, globalPtr);
            return nameToHash;
        }

        const auto tableArray = *reinterpret_cast<const std::uintptr_t *>(
            globalPtr + cfg.tableArrayOffset);
        if (tableArray < 0x10000)
        {
            logger.warning(
                "{}: tableArray is null/invalid (0x{:X}) -- offset 0x{:X} "
                "inside globalPtr may have moved",
                cfg.logLabel, tableArray,
                static_cast<std::int64_t>(cfg.tableArrayOffset));
            return nameToHash;
        }

        logger.trace(
            "{}: globalPtr=0x{:X} tableArray=0x{:X} range=0x{:X}-0x{:X}",
            cfg.logLabel, globalPtr, tableArray,
            cfg.tableScanMin, cfg.tableScanMax);

        const auto t0 = std::chrono::steady_clock::now();
        std::uint32_t entries = 0;
        char buf[k_maxStringLen + 1];

        for (std::uint32_t hash = cfg.tableScanMin;
             hash <= cfg.tableScanMax; ++hash)
        {
            const auto len = read_table_entry(
                tableArray, hash, buf, sizeof(buf));
            if (len == 0 || len >= k_maxStringLen)
                continue;
            if (!matches_prefix(buf, len, cfg.prefix))
                continue;

            nameToHash[std::string(buf, len)] = hash;
            ++entries;
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count();

        if (entries == 0)
        {
            logger.warning(
                "{}: 0 entries matching prefix '{}' found in range "
                "0x{:X}..0x{:X} -- table not yet populated or prefix "
                "missing from this build; deferring feature",
                cfg.logLabel, cfg.prefix ? cfg.prefix : "",
                cfg.tableScanMin, cfg.tableScanMax);
        }
        else
        {
            logger.info(
                "{}: {} entries for prefix '{}' in range 0x{:X}..0x{:X} "
                "in {}ms",
                cfg.logLabel, entries, cfg.prefix ? cfg.prefix : "",
                cfg.tableScanMin, cfg.tableScanMax, ms);
        }

        return nameToHash;
    }

} // namespace CDCore
