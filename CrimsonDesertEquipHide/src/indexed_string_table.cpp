#include "indexed_string_table.hpp"
#include "categories.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <chrono>
#include <cstring>

namespace EquipHide
{
    static constexpr uint32_t k_tableScanMin = 0xAC00;
    static constexpr uint32_t k_tableScanMax = 0xCFFF;

    static constexpr std::size_t k_maxStringLen = 64;

    static std::size_t read_table_entry(uintptr_t tableArray, uint32_t hash,
                                        char *buf, std::size_t bufSize) noexcept
    {
        __try
        {
            const auto entryAddr = tableArray + static_cast<uintptr_t>(hash) * 16;
            const auto strPtr = *reinterpret_cast<const uintptr_t *>(entryAddr);
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

    std::unordered_map<std::string, uint32_t> scan_indexed_string_table(
        uintptr_t mapLookupFunc)
    {
        auto &logger = DMK::Logger::get_instance();
        std::unordered_map<std::string, uint32_t> nameToHash;

        const auto ripInstr = mapLookupFunc + 20;
        const auto *instrBytes = reinterpret_cast<const uint8_t *>(ripInstr);

        if (instrBytes[0] != 0x48 || instrBytes[1] != 0x8B || instrBytes[2] != 0x05)
        {
            logger.warning("IndexedStringA scan: unexpected opcode at +20 "
                           "(expected 48 8B 05, got {:02X} {:02X} {:02X})",
                           instrBytes[0], instrBytes[1], instrBytes[2]);
            return nameToHash;
        }

        int32_t disp = 0;
        std::memcpy(&disp, instrBytes + 3, sizeof(int32_t));
        const auto instrEnd = ripInstr + 7;
        const auto globalPtrAddr = static_cast<uintptr_t>(
            static_cast<int64_t>(instrEnd) + disp);

        const auto globalPtr = *reinterpret_cast<const uintptr_t *>(globalPtrAddr);
        if (globalPtr < 0x10000)
        {
            logger.trace("IndexedStringA scan: global pointer not yet initialized (0x{:X})",
                         globalPtr);
            return nameToHash;
        }

        const auto tableArray = *reinterpret_cast<const uintptr_t *>(globalPtr + 0x58);
        if (tableArray < 0x10000)
        {
            logger.warning("IndexedStringA scan: tableArray is null/invalid (0x{:X})",
                           tableArray);
            return nameToHash;
        }

        logger.trace("IndexedStringA scan: globalPtr=0x{:X} tableArray=0x{:X} "
                     "range=0x{:X}-0x{:X}",
                     globalPtr, tableArray, k_tableScanMin, k_tableScanMax);

        const auto t0 = std::chrono::steady_clock::now();
        uint32_t cdEntries = 0;
        uint32_t probeHits = 0;

        char buf[k_maxStringLen + 1];

        for (uint32_t hash = k_tableScanMin; hash <= k_tableScanMax; ++hash)
        {
            auto len = read_table_entry(tableArray, hash, buf, sizeof(buf));
            if (len == 0 || len >= k_maxStringLen)
                continue;

            if (buf[0] != 'C' || buf[1] != 'D' || buf[2] != '_')
                continue;

            nameToHash[std::string(buf, len)] = hash;
            ++cdEntries;
        }

        auto unresolvedNames = get_unresolved_parts(nameToHash);
        if (!unresolvedNames.empty())
        {
            constexpr uint32_t k_wideScanMax = 0x20000;
            for (uint32_t h = 1; h < k_wideScanMax && !unresolvedNames.empty(); ++h)
            {
                if (h >= k_tableScanMin && h <= k_tableScanMax)
                    continue;

                auto len = read_table_entry(tableArray, h, buf, sizeof(buf));
                if (len == 0 || len >= k_maxStringLen)
                    continue;
                if (buf[0] != 'C' || buf[1] != 'D' || buf[2] != '_')
                    continue;

                for (auto it = unresolvedNames.begin();
                     it != unresolvedNames.end(); ++it)
                {
                    if (len == it->size() &&
                        std::memcmp(buf, it->c_str(), len) == 0)
                    {
                        nameToHash[*it] = h;
                        ++probeHits;
                        logger.trace("Wide scan: {} found at 0x{:X}", *it, h);
                        unresolvedNames.erase(it);
                        break;
                    }
                }
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        logger.trace("IndexedStringA scan complete: {} CD_ entries ({} range, {} probed) in {}ms",
                     cdEntries + probeHits, cdEntries, probeHits, ms);

        return nameToHash;
    }

} // namespace EquipHide
