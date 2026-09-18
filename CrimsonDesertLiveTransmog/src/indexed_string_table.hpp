#ifndef TRANSMOG_INDEXED_STRING_TABLE_HPP
#define TRANSMOG_INDEXED_STRING_TABLE_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Transmog
{
    /**
     * @brief Scan the IndexedStringA global table for "CD_"-prefixed name -> hash mappings.
     *
     * LiveTransmog only needs the slot part names (CD_Helm, CD_Upperbody, CD_Cloak, CD_Hand, CD_Foot), so the default
     * CDCore config (primary range 0xAC00..0xCFFF, no wide-scan) is sufficient.
     */
    [[nodiscard]] std::unordered_map<std::string, std::uint32_t>
    scan_indexed_string_table(std::uintptr_t map_lookup_func);

} // namespace Transmog

#endif // TRANSMOG_INDEXED_STRING_TABLE_HPP
