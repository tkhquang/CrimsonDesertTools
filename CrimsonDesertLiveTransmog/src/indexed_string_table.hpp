#pragma once

// Thin wrapper over CDCore::scan_indexed_string_table. Preserves the
// existing free-function signature so call sites (transmog.cpp,
// part_show_suppress.cpp, etc.) don't need to change.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Transmog
{
    /**
     * @brief Scan the IndexedStringA global table for "CD_"-prefixed
     *        name -> hash mappings.
     *
     * LiveTransmog only needs the slot part names (CD_Helm, CD_Upperbody,
     * CD_Cloak, CD_Hand, CD_Foot), so the default CDCore config (primary
     * range 0xAC00..0xCFFF, no wide-scan) is sufficient.
     */
    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(
        std::uintptr_t mapLookupFunc);

} // namespace Transmog
