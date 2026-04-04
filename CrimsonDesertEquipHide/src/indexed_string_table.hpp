#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace EquipHide
{
    /**
     * @brief Scan the IndexedStringA table for name-to-hash mappings.
     *
     * Table layout:
     *   globalPtr   = *(qword*)(mapLookupFunc + 20 + rip_disp)
     *   tableArray  = *(qword*)(globalPtr + 0x58)
     *   entry[hash] = tableArray + hash * 16
     *   entry[hash]+0 = pointer to null-terminated string (or 0)
     */
    std::unordered_map<std::string, uint32_t> scan_indexed_string_table(
        uintptr_t mapLookupFunc);

} // namespace EquipHide
