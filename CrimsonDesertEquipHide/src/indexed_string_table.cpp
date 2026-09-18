#include "indexed_string_table.hpp"

#include <cdcore/indexed_string_table.hpp>

namespace EquipHide
{
    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(std::uintptr_t map_lookup_func)
    {
        const CDCore::IndexedStringScanConfig config{
            .log_label = "IndexedStringA scan [EH]",
        };
        return CDCore::scan_indexed_string_table(map_lookup_func, config);
    }

} // namespace EquipHide
