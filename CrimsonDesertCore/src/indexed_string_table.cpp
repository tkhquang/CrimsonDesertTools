#include "cdcore/indexed_string_table.hpp"

#include <DetourModKit/logger.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/scan.hpp>

#include <excpt.h>

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

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
         * @brief Collisions already reported, keyed by name and both hashes.
         * @details The deferred and lazy scans repeat the sweep for the life of the process. The set keeps each
         *          collision to one warning. A new stale entry for the same name still reports, because its hash is
         *          part of the key.
         */
        std::mutex s_reported_collisions_mutex;
        std::unordered_set<std::string> s_reported_collisions;

        /** @brief True the first time this exact collision is seen in this process. */
        bool first_report_of(std::string_view name, std::uint32_t bare_hash, std::uint32_t prefixed_hash)
        {
            std::string key{name};
            key += '@';
            key += std::to_string(bare_hash);
            key += '/';
            key += std::to_string(prefixed_hash);
            std::lock_guard<std::mutex> lock(s_reported_collisions_mutex);
            return s_reported_collisions.insert(std::move(key)).second;
        }

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

        // With an unprefixed-alias filter, every entry has to be copied out so the filter can see it, so the read
        // drops its prefix gate and the prefix test moves into the loop.
        const bool alias_unprefixed = cfg.accept_unprefixed != nullptr && !prefix.empty();
        const std::string_view read_prefix = alias_unprefixed ? std::string_view{} : prefix;

        const auto scan_start = std::chrono::steady_clock::now();
        std::uint32_t aliased = 0;
        char buf[MAX_STRING_LEN + 1];
        std::string alias_key;

        // Unprefixed entries collect apart from the prefixed ones, so the merge below decides between the two
        // spellings of one name regardless of which hash the sweep reaches first.
        std::unordered_map<std::string, std::uint32_t> unprefixed_to_hash;

        for (std::uint32_t hash = cfg.table_scan_min; hash <= cfg.table_scan_max; ++hash)
        {
            const auto len = read_table_entry(table_array, hash, read_prefix, buf, sizeof(buf));
            if (len == 0 || len >= MAX_STRING_LEN)
                continue;

            const std::string_view name{buf, len};
            if (name.starts_with(prefix))
            {
                name_to_hash[std::string(name)] = hash;
                continue;
            }

            alias_key.assign(prefix);
            alias_key.append(name);
            if (cfg.accept_unprefixed(alias_key))
                unprefixed_to_hash.try_emplace(alias_key, hash);
        }

        // The unprefixed spelling wins. The engine's own part names carry no prefix, so when both spellings are
        // interned, the prefixed one came from data that still uses the old names, usually a mod prefab built for an
        // older game version. That entry carries its own hash, and every character shares the hash table. A win for
        // the prefixed entry therefore redirects the part for all of them, and the feature stops everywhere at once.
        for (const auto &[key, hash] : unprefixed_to_hash)
        {
            ++aliased;
            const auto [it, inserted] = name_to_hash.try_emplace(key, hash);
            if (inserted || it->second == hash)
                continue;

            const auto prefixed_hash = it->second;
            it->second = hash;
            if (first_report_of(key, hash, prefixed_hash))
            {
                logger.warning(
                    "{}: '{}' is interned both without the prefix (0x{:X}) and with it (0x{:X}). Using 0x{:X}, the "
                    "engine's part name. The prefixed copy comes from data that still uses the old part names, "
                    "usually an outdated mod prefab, and items built from that data are not matched",
                    cfg.log_label,
                    key,
                    hash,
                    prefixed_hash,
                    hash
                );
            }
        }
        const auto entries = name_to_hash.size();

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
                "{}: {} entries for prefix '{}' ({} interned without it) in range 0x{:X}..0x{:X} in {}ms",
                cfg.log_label,
                entries,
                prefix,
                aliased,
                cfg.table_scan_min,
                cfg.table_scan_max,
                ms
            );
        }

        return name_to_hash;
    }

} // namespace CDCore
