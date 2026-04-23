#include "indexed_string_table.hpp"

#include <cdcore/indexed_string_table.hpp>

namespace EquipHide
{
    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(
        std::uintptr_t mapLookupFunc)
    {
        CDCore::IndexedStringScanConfig cfg;
        cfg.logLabel = "IndexedStringA scan [EH]";
        return CDCore::scan_indexed_string_table(mapLookupFunc, cfg);
    }

} // namespace EquipHide
