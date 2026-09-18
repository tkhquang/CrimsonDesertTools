#ifndef EQUIPHIDE_INDEXED_STRING_TABLE_HPP
#define EQUIPHIDE_INDEXED_STRING_TABLE_HPP

#include <cstdint>
#include <string>
#include <unordered_map>

namespace EquipHide
{
    /**
     * @brief Scan the IndexedStringA table for "CD_"-prefixed name to hash mappings under the EquipHide log label.
     * @details Delegates to CDCore::scan_indexed_string_table, which owns the table layout and the SEH-guarded reads.
     * @param map_lookup_func Address of the engine map-lookup function that carries the RIP anchor to the table.
     * @return The resolved name to hash map, empty when the table is not reachable yet.
     */
    [[nodiscard]] std::unordered_map<std::string, std::uint32_t>
    scan_indexed_string_table(std::uintptr_t map_lookup_func);

} // namespace EquipHide

#endif // EQUIPHIDE_INDEXED_STRING_TABLE_HPP
