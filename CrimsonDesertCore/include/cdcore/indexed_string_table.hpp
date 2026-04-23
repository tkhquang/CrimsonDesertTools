#ifndef CDCORE_INDEXED_STRING_TABLE_HPP
#define CDCORE_INDEXED_STRING_TABLE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

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
// walks the chain, then enumerates the configured hash range end-to-end
// in a single pass and returns every entry whose string starts with
// cfg.prefix.
// ---------------------------------------------------------------------------

namespace CDCore
{
    struct IndexedStringScanConfig
    {
        // Name-prefix filter (entries whose string does NOT start with this
        // are skipped). "CD_" matches every Crimson Desert slot part name.
        const char *prefix = "CD_";

        // Hash range to sweep. The default covers the full observed bucket
        // space; callers should only narrow it when a specific patch is
        // known to keep the relevant entries inside a tighter window and
        // scan cost is a concern. Per-version bucket drift (seen between
        // v1.02.00, v1.03.01 and v1.04.00) means any narrow default has
        // to be revisited every major patch; keeping it wide is the
        // self-healing choice.
        std::uint32_t tableScanMin = 1;
        std::uint32_t tableScanMax = 0x1FFFF;

        // Offset of the table_array pointer inside the globalPtr struct.
        // Runtime-data layout, resolved by IDA on v1.02.00. No AOB.
        std::ptrdiff_t tableArrayOffset = 0x58;

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
