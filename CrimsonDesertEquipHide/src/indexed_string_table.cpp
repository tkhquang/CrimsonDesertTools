#include "indexed_string_table.hpp"
#include "categories.hpp"

#include <cdcore/indexed_string_table.hpp>

namespace EquipHide
{
    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(
        std::uintptr_t mapLookupFunc)
    {
        CDCore::IndexedStringScanConfig cfg;
        cfg.logLabel = "IndexedStringA scan [EH]";
        // Wide-scan fallback: any CD_ part declared in categories.hpp
        // that didn't land in the primary bucket range gets probed over
        // the extended range. Keeps us resilient to future hash-bucket
        // reshuffles that move a category name outside 0xAC00..0xCFFF.
        cfg.wideScanMax = 0x20000;
        cfg.getUnresolved =
            [](const std::unordered_map<std::string, std::uint32_t> &primary) {
                return get_unresolved_parts(primary);
            };

        return CDCore::scan_indexed_string_table(mapLookupFunc, cfg);
    }

} // namespace EquipHide
