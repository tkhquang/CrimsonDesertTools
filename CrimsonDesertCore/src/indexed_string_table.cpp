#include "cdcore/indexed_string_table.hpp"

#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/scan.hpp>

#include <excpt.h>

#include <chrono>
#include <string_view>

namespace CDCore
{
    namespace
    {
        /** @brief Longest string the scanner copies out of one table entry. */
        constexpr std::size_t MAX_STRING_LEN = 64;

        /** @brief Byte stride between consecutive table entries. */
        constexpr std::uintptr_t ENTRY_STRIDE = 16;

        /** @brief Prologue window the scanner sweeps for the `mov rax, [rip+disp32]` that names the global. */
        constexpr std::size_t RIP_ANCHOR_SEARCH_BYTES = 0x40;

        /**
         * @brief Copy entry[hash]'s string into @p buf, but only when it starts with @p prefix.
         *
         * @details The prefix test runs before the copy on purpose. The table holds the engine's entire
         *          interned-string set, tens of thousands of entries, and only a few hundred carry the caller's
         *          prefix. A copy-then-filter pass spends a full string read on entries that were never candidates. A
         *          compare of the leading bytes first keeps a rejected entry down to the one cache line its string
         *          starts on.
         *
         * @return Copied length, or 0 for an empty slot, a prefix mismatch, or a faulting read. Callers cannot
         *         distinguish the three and do not need to.
         */
        std::size_t read_table_entry(
            std::uintptr_t table_array,
            std::uint32_t hash,
            std::string_view prefix,
            char *buf,
            std::size_t buf_size
        ) noexcept
        {
            __try
            {
                const auto entry_addr = table_array + static_cast<std::uintptr_t>(hash) * ENTRY_STRIDE;
                const auto str_ptr = *reinterpret_cast<const std::uintptr_t *>(entry_addr);
                if (!DMK::memory::is_plausible_ptr(DMK::Address{str_ptr}))
                    return 0;

                const auto *src = reinterpret_cast<const char *>(str_ptr);

                // A test of the terminator alongside the mismatch is what stops the compare one byte past the end of
                // a string shorter than the prefix. An empty prefix skips the loop, so every entry is a candidate.
                for (std::size_t i = 0; i < prefix.size(); ++i)
                {
                    const char c = src[i];
                    if (c == '\0' || c != prefix[i])
                        return 0;
                }

                std::size_t len = 0;
                while (len < buf_size - 1 && src[len] != '\0')
                {
                    buf[len] = src[len];
                    ++len;
                }
                buf[len] = '\0';
                return len;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

    } // namespace

    std::unordered_map<std::string, std::uint32_t>
    scan_indexed_string_table(std::uintptr_t map_lookup_func, const IndexedStringScanConfig &cfg)
    {
        auto &logger = DMK::log();
        std::unordered_map<std::string, std::uint32_t> name_to_hash;

        if (!map_lookup_func)
            return name_to_hash;

        // Locate `mov rax, [rip+disp32]` inside the first 0x40 bytes of map_lookup_func and resolve its target. The
        // resolver skips a decoy occurrence whose displacement lands on an implausible or unreadable address and
        // reports the last concrete decode failure, so a compiler shuffle that moves the real instruction later in the
        // prologue still resolves. Global uniqueness is irrelevant here: the search window is one known function.
        const auto resolved = DMK::scan::find_and_resolve_rip_relative(
            DMK::Region{DMK::Address{map_lookup_func}, RIP_ANCHOR_SEARCH_BYTES},
            DMK::scan::PREFIX_MOV_RAX_RIP,
            7
        );
        if (!resolved)
        {
            logger.warning(
                "{}: `48 8B 05` rip-instruction did not resolve in the first 0x{:X} bytes of "
                "mapLookupFunc (0x{:X}): {}",
                cfg.log_label,
                RIP_ANCHOR_SEARCH_BYTES,
                map_lookup_func,
                resolved.error().message()
            );
            return name_to_hash;
        }
        const auto global_ptr_addr = resolved->raw();

        // The resolved slot is a RIP-relative module address. Guard the read so a build whose layout shifted it
        // outside committed memory yields 0 (handled as "not yet initialized") rather than a fault.
        const auto global_ptr = DMK::memory::read<std::uintptr_t>(DMK::Address{global_ptr_addr}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{global_ptr}))
        {
            logger.trace("{}: global pointer not yet initialized (0x{:X})", cfg.log_label, global_ptr);
            return name_to_hash;
        }

        // The global is a live game heap pointer that can tear or relocate across a world reload. A faulting read
        // yields 0 and routes to the "offset moved" warning below instead of a crash in the caller.
        const auto table_array =
            DMK::memory::read<std::uintptr_t>(DMK::Address{global_ptr + cfg.table_array_offset}).value_or(0);
        if (!DMK::memory::is_plausible_ptr(DMK::Address{table_array}))
        {
            logger.warning(
                "{}: tableArray is null/invalid (0x{:X}) - offset 0x{:X} inside globalPtr may have moved",
                cfg.log_label,
                table_array,
                static_cast<std::int64_t>(cfg.table_array_offset)
            );
            return name_to_hash;
        }

        logger.trace(
            "{}: globalPtr=0x{:X} tableArray=0x{:X} range=0x{:X}-0x{:X}",
            cfg.log_label,
            global_ptr,
            table_array,
            cfg.table_scan_min,
            cfg.table_scan_max
        );

        // A view is never null, so the old null guard is gone and an empty prefix matches every entry.
        const std::string_view prefix = cfg.prefix;

        const auto scan_start = std::chrono::steady_clock::now();
        std::uint32_t entries = 0;
        char buf[MAX_STRING_LEN + 1];

        for (std::uint32_t hash = cfg.table_scan_min; hash <= cfg.table_scan_max; ++hash)
        {
            const auto len = read_table_entry(table_array, hash, prefix, buf, sizeof(buf));
            if (len == 0 || len >= MAX_STRING_LEN)
                continue;

            name_to_hash[std::string(buf, len)] = hash;
            ++entries;
        }

        const auto scan_end = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - scan_start).count();

        if (entries == 0)
        {
            logger.warning(
                "{}: 0 entries matching prefix '{}' found in range "
                "0x{:X}..0x{:X} - table not yet populated or prefix missing from this build; deferring feature",
                cfg.log_label,
                prefix,
                cfg.table_scan_min,
                cfg.table_scan_max
            );
        }
        else
        {
            // Per-scan summary stays at TRACE so the deferred-scan poll path (called every 2s until table stability)
            // does not flood the INFO stream. Call sites that want a one-shot INFO line emit their own at init-time
            // decision points.
            logger.trace(
                "{}: {} entries for prefix '{}' in range 0x{:X}..0x{:X} in {}ms",
                cfg.log_label,
                entries,
                prefix,
                cfg.table_scan_min,
                cfg.table_scan_max,
                ms
            );
        }

        return name_to_hash;
    }

} // namespace CDCore
