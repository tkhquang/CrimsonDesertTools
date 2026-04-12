#include "indexed_string_table.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <chrono>
#include <cstring>

namespace Transmog
{
    // CD_* strings live in this bucket range on v1.02.00. Scanning a
    // wider range is unnecessary — live-transmog only needs the slot
    // part names (CD_Helm, CD_Upperbody, CD_Cloak, CD_Hand, CD_Foot).
    // If a future patch reshuffles the bucket layout, the summary log
    // below will emit a WARNING when the scan yields 0 entries, making
    // the breakage immediately visible.
    static constexpr uint32_t k_tableScanMin = 0xAC00;
    static constexpr uint32_t k_tableScanMax = 0xCFFF;
    static constexpr uint32_t k_expectedCdEntries = 600; // approx on v1.02.00

    static constexpr std::size_t k_maxStringLen = 64;

    // IndexedStringA table_array offset within the global container.
    // Source: IDA decompile of mapLookupFunc (k_mapLookupCandidates in
    // aob_resolver.hpp) on v1.02.00. Runtime data layout — no AOB.
    static constexpr std::ptrdiff_t k_indexedStringATableArrayOffset = 0x58;

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

        if (!mapLookupFunc)
            return nameToHash;

        // The mapLookup prologue historically carries `48 8B 05 <disp32>`
        // (mov rax, [rip+disp]) at fixed offset +20. We replace the
        // hardcoded offset with a bounded Scanner pass for future-
        // patch safety: scan the first 0x40 bytes of mapLookupFunc for
        // the specific 3-byte opcode. Since the scan is bounded to THIS
        // function only, global uniqueness is irrelevant.
        const auto funcStart = reinterpret_cast<const std::byte *>(mapLookupFunc);
        auto ripAob = DMK::Scanner::parse_aob("48 8B 05 ?? ?? ?? ??");
        if (!ripAob)
        {
            logger.warning("IndexedStringA scan: parse_aob failed for mapLookup rip-anchor");
            return nameToHash;
        }
        const auto *ripMatch = DMK::Scanner::find_pattern(funcStart, 0x40, *ripAob);
        if (!ripMatch)
        {
            logger.warning("IndexedStringA scan: `48 8B 05` rip-instruction not found "
                           "in first 0x40 bytes of mapLookupFunc (0x{:X})",
                           mapLookupFunc);
            return nameToHash;
        }
        const auto ripInstr = reinterpret_cast<uintptr_t>(ripMatch);

        int32_t disp = 0;
        std::memcpy(&disp, reinterpret_cast<const void *>(ripInstr + 3), sizeof(int32_t));
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

        const auto tableArray = *reinterpret_cast<const uintptr_t *>(
            globalPtr + k_indexedStringATableArrayOffset);
        if (tableArray < 0x10000)
        {
            logger.warning("IndexedStringA scan: tableArray is null/invalid (0x{:X}) "
                           "— offset 0x{:X} inside globalPtr may have moved",
                           tableArray, k_indexedStringATableArrayOffset);
            return nameToHash;
        }

        logger.trace("IndexedStringA scan: globalPtr=0x{:X} tableArray=0x{:X} "
                     "range=0x{:X}-0x{:X}",
                     globalPtr, tableArray, k_tableScanMin, k_tableScanMax);

        const auto t0 = std::chrono::steady_clock::now();
        uint32_t cdEntries = 0;
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

        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (cdEntries == 0)
        {
            logger.warning("IndexedStringA scan: 0 CD_ entries found in range "
                           "0x{:X}..0x{:X} (expected ~{} on v1.02.00) — hash "
                           "bucket layout may have shifted, deferring feature",
                           k_tableScanMin, k_tableScanMax, k_expectedCdEntries);
        }
        else
        {
            logger.info("IndexedStringA scan complete: {} CD_ entries in "
                        "range 0x{:X}..0x{:X} (expected ~{}) in {}ms",
                        cdEntries, k_tableScanMin, k_tableScanMax,
                        k_expectedCdEntries, ms);
        }

        return nameToHash;
    }

} // namespace Transmog
