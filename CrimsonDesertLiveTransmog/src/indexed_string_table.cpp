#include "indexed_string_table.hpp"

#include "slot_metadata.hpp"

#include <cdcore/indexed_string_table.hpp>

#include <string_view>

namespace Transmog
{
    namespace
    {
        // The slot part names the mod looks up (SLOT_METADATA's part_show_hash_key column). The scan adopts an entry
        // the engine interned without its `CD_` prefix only when its prefixed spelling is one of these.
        bool is_slot_part_name(std::string_view prefixed_name)
        {
            for (const auto &meta : SLOT_METADATA)
            {
                if (meta.part_show_hash_key != nullptr && prefixed_name == meta.part_show_hash_key)
                    return true;
            }
            return false;
        }
    } // namespace

    std::unordered_map<std::string, std::uint32_t> scan_indexed_string_table(std::uintptr_t map_lookup_func)
    {
        CDCore::IndexedStringScanConfig cfg;
        cfg.log_label = "IndexedStringA scan [LT]";
        cfg.accept_unprefixed = &is_slot_part_name;
        return CDCore::scan_indexed_string_table(map_lookup_func, cfg);
    }

} // namespace Transmog
