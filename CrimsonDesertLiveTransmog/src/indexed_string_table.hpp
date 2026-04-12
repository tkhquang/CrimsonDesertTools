#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Transmog
{
    /**
     * @brief Scan the IndexedStringA global table for name-to-hash mappings.
     *
     * Table layout (resolved from mapLookupFunc + 20 `mov rax, [rip+disp]`):
     *   globalPtr   = *(qword*)(mapLookupFunc + 20 + rip_disp)
     *   tableArray  = *(qword*)(globalPtr + 0x58)
     *   entry[hash] = tableArray + hash * 16
     *   entry[hash]+0 = pointer to null-terminated string (or 0)
     *
     * Returns the subset of entries whose string begins with "CD_" — this is
     * the only prefix live-transmog cares about (slot part names).
     *
     * Returns empty map if the global pointer is not yet initialized or if
     * the mapLookup prologue doesn't match the expected shape.
     */
    std::unordered_map<std::string, uint32_t> scan_indexed_string_table(
        uintptr_t mapLookupFunc);

} // namespace Transmog
