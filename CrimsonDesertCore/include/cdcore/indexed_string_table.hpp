#ifndef CDCORE_INDEXED_STRING_TABLE_HPP
#define CDCORE_INDEXED_STRING_TABLE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// IndexedStringA table scanner.
//
// Table layout (resolved from mapLookupFunc prologue `mov rax, [rip+disp]`):
//   globalPtr   = *(qword*)(rip-relative target)
//   tableArray  = *(qword*)(globalPtr + tableArrayOffset)
//   entry[hash] = tableArray + hash * 16
//   entry[hash]+0 = pointer to null-terminated string (or 0)
//
// The scanner locates the `48 8B 05 <disp32>` instruction inside the first
// 0x40 bytes of mapLookupFunc (patch-proof against compiler shuffles),
// walks the chain, then enumerates the configured hash range and returns
// every entry whose string starts with cfg.prefix.
//
// Optional wide-scan fallback: if cfg.getUnresolved is set, after the
// primary range scan the scanner calls it with the partial map and
// searches the extended range [1, cfg.wideScanMax) for the returned
// unresolved names (skipping the primary range to avoid redundant work).
// ---------------------------------------------------------------------------

namespace CDCore
{
    struct IndexedStringScanConfig
    {
        // Name-prefix filter (entries whose string does NOT start with this
        // are skipped). "CD_" matches every Crimson Desert slot part name.
        const char *prefix = "CD_";

        // Primary hash range -- entries in this range are always scanned.
        std::uint32_t tableScanMin = 0xAC00;
        std::uint32_t tableScanMax = 0xCFFF;

        // Approximate number of matching entries on v1.02.00. Used only to
        // colour the end-of-scan log line ("expected ~N entries").
        std::uint32_t expectedEntries = 600;

        // Offset of the table_array pointer inside the globalPtr struct.
        // Runtime-data layout, resolved by IDA on v1.02.00. No AOB.
        std::ptrdiff_t tableArrayOffset = 0x58;

        // Optional wide-scan fallback. If set AND wideScanMax > 0, the
        // scanner calls getUnresolved(primaryResults) after the primary
        // range pass; each returned name is then probed across the
        // extended range [1, wideScanMax) (skipping the primary range).
        // Leave unset to disable the fallback.
        std::uint32_t wideScanMax = 0;
        std::function<std::vector<std::string>(
            const std::unordered_map<std::string, std::uint32_t> &)>
            getUnresolved;

        // Label used in log lines. Change it to distinguish per-mod scans
        // in the shared log stream.
        const char *logLabel = "IndexedStringA scan";
    };

    /**
     * @brief Scan the IndexedStringA global table for name -> hash mappings.
     *
     * Returns an empty map if:
     *   - mapLookupFunc is 0
     *   - the `48 8B 05` RIP anchor is not found in the first 0x40 bytes
     *   - the resolved global pointer or tableArray is null/not-yet-init
     *
     * All reads of the live table are SEH-guarded.
     */
    [[nodiscard]] std::unordered_map<std::string, std::uint32_t>
    scan_indexed_string_table(
        std::uintptr_t mapLookupFunc,
        const IndexedStringScanConfig &cfg = {});

} // namespace CDCore

#endif // CDCORE_INDEXED_STRING_TABLE_HPP
