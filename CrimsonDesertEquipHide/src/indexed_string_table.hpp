#pragma once

// Thin EquipHide-flavored wrapper over CDCore::scan_indexed_string_table.
// Preserves the existing free-function signature so call sites in
// background_threads.cpp / equip_hide.cpp don't need to change; the
// implementation delegates to CDCore with an EquipHide-specific
// unresolved-names callback (wide-scan fallback over categories.hpp).

#include <cstdint>
#include <string>
#include <unordered_map>

namespace EquipHide
{
    /**
     * @brief Scan the IndexedStringA table for "CD_"-prefixed name -> hash
     *        mappings, with an EquipHide-specific wide-scan fallback for
     *        any part names declared in categories.hpp that didn't land
     *        in the primary bucket range.
     *
     * Table layout:
     *   globalPtr   = *(qword*)(mapLookupFunc + rip-anchor target)
     *   tableArray  = *(qword*)(globalPtr + 0x58)
     *   entry[hash] = tableArray + hash * 16
     *   entry[hash]+0 = pointer to null-terminated string (or 0)
     */
    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(
        std::uintptr_t mapLookupFunc);

} // namespace EquipHide
