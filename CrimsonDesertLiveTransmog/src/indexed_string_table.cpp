#include "indexed_string_table.hpp"

#include <cdcore/indexed_string_table.hpp>

namespace Transmog
{
    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(std::uintptr_t map_lookup_func)
    {
        CDCore::IndexedStringScanConfig cfg;
        cfg.log_label = "IndexedStringA scan [LT]";
        return CDCore::scan_indexed_string_table(map_lookup_func, cfg);
    }

} // namespace Transmog
