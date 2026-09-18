#ifndef CDCORE_INDEXED_STRING_TABLE_HPP
#define CDCORE_INDEXED_STRING_TABLE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

// IndexedStringA table scanner.
//
// Table layout (resolved from mapLookupFunc prologue `mov rax, [rip+disp]`):
//   globalPtr   = *(qword*)(rip-relative target)
//   tableArray  = *(qword*)(globalPtr + tableArrayOffset)
//   entry[hash] = tableArray + hash * 16
//   entry[hash]+0 = pointer to null-terminated string (or 0)
//
// The scanner locates the `48 8B 05 <disp32>` instruction inside the first 0x40 bytes of mapLookupFunc (patch-proof
// against compiler shuffles), walks the chain, then enumerates the configured hash range end-to-end in a single pass
// and returns every entry whose string starts with cfg.prefix.

namespace CDCore
{
    /**
     * @brief Knobs for one IndexedStringA table sweep.
     * @details Every field defaults to a Crimson Desert slot-part scan, so a caller overrides only what differs. The
     *          default hash range covers the full observed bucket space. Narrow it only when a patch is known to keep
     *          the relevant entries inside a tighter window and scan cost matters. Per-version bucket drift means a
     *          narrow default must be revisited every major patch, so a wide range is the self-healing choice.
     */
    struct IndexedStringScanConfig
    {
        /**
         * @brief Name-prefix filter.
         * @details The scan skips an entry whose string does not start with this. "CD_" matches every Crimson Desert
         *          slot part name.
         */
        const char *prefix = "CD_";

        /// Inclusive low end of the hash range the sweep covers.
        std::uint32_t tableScanMin = 1;
        /// Inclusive high end of the hash range the sweep covers.
        std::uint32_t tableScanMax = 0x1FFFF;

        /**
         * @brief Offset of the table-array pointer inside the global struct.
         * @details Runtime-data layout offset with no AOB behind it. Check it after a major patch.
         */
        std::ptrdiff_t tableArrayOffset = 0x58;

        /// Label used in log lines. Change it to tell per-mod scans apart in the shared log stream.
        const char *logLabel = "IndexedStringA scan";
    };

    /**
     * @brief Scan the IndexedStringA global table for name -> hash mappings.
     * @param mapLookupFunc Engine map-lookup function whose prologue names the IndexedStringA global.
     * @param cfg Scan knobs. The defaults cover a Crimson Desert slot-part sweep.
     * @return Every matching name mapped to its hash. The map is empty when:
     *         - mapLookupFunc is 0,
     *         - the `48 8B 05` RIP anchor is absent from the first 0x40 bytes,
     *         - the resolved global pointer or table array is null or not yet initialized.
     * @note All reads of the live table are SEH-guarded.
     */
    [[nodiscard]] std::unordered_map<std::string, std::uint32_t>
    scan_indexed_string_table(std::uintptr_t mapLookupFunc, const IndexedStringScanConfig &cfg = {});

} // namespace CDCore

#endif // CDCORE_INDEXED_STRING_TABLE_HPP
