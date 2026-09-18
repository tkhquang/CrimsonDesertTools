#include "part_show_suppress.hpp"
#include "shared_state.hpp"
#include "slot_metadata.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Transmog::part_show_suppress
{
    namespace
    {
        // Suppression set, holding the IndexedStringA hashes the current mask suppresses. A slot contributes at most
        // four hashes, so the whole live set fits in a cache line or two and the detour answers with a short scan of
        // the published prefix. The comparison is on the full 32-bit hash, so two hashes that share a low half stay
        // independent.
        constexpr std::size_t MAX_SUPPRESSED = SLOT_COUNT * 4;
        std::array<std::atomic<std::uint32_t>, MAX_SUPPRESSED> s_suppressed_hashes{};
        std::atomic<std::size_t> s_suppressed_count{0};

        std::atomic<PartAddShowFn> s_original_part_add_show{nullptr};

        // Per-slot IndexedStringA indices. IndexedStringA buckets rebuild on every game patch, so init_slot_hashes()
        // resolves these at runtime from a prior scan_indexed_string_table() pass.
        struct SlotHashes
        {
            std::size_t slot{};
            // Zero-terminated. An unused entry holds 0.
            std::array<std::uint32_t, 4> hashes{};
        };

        // The (TransmogSlot -> "CD_*") slot-name table comes from slot_metadata.hpp's `part_show_hash_key` field. Only
        // the 5 armor slots carry CD_* slot-suffix hashes in IndexedStringA. Accessory and weapon slots store
        // nullptr there and the loop below filters them out. A row without a part_show_hash_key keeps all-zero
        // hashes and acts as a no-op, which is correct: accessories do not use this hash-driven path.
        //
        // s_slot_hashes stays sized to SLOT_COUNT because callers index it by TransmogSlot value, which can be any of
        // the entries.
        std::array<SlotHashes, SLOT_COUNT> s_slot_hashes{};
        std::atomic<bool> s_slot_hashes_ready{false};

        // Diagnostic: remember which 16-bit hashes the detour has already logged so each unique hash shows up once per
        // session. A direct-indexed byte table keeps the lookup O(1) and branch-free on the dispatch path, and the
        // low-half aliasing costs nothing beyond a missing trace line for a colliding pair.
        std::atomic<std::uint8_t> s_hash_seen[0x10000]{};

        [[nodiscard]] std::uint32_t read_hash_safe(std::uint64_t part_hash_ptr) noexcept
        {
            return DMK::memory::read<std::uint32_t>(DMK::Address{static_cast<std::uintptr_t>(part_hash_ptr)})
                .value_or(0);
        }

        [[nodiscard]] bool hash_suppressed(std::uint32_t part_hash) noexcept
        {
            const auto live = s_suppressed_count.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < live; ++i)
            {
                if (s_suppressed_hashes[i].load(std::memory_order_relaxed) == part_hash)
                    return true;
            }
            return false;
        }
    } // namespace

    void set_part_add_show_trampoline(PartAddShowFn original)
    {
        s_original_part_add_show.store(original, std::memory_order_release);
    }

    void set_hash_suppressed(std::uint32_t part_hash, bool suppressed) noexcept
    {
        if (part_hash == 0)
            return;

        const auto live = s_suppressed_count.load(std::memory_order_relaxed);
        std::size_t at = MAX_SUPPRESSED;
        for (std::size_t i = 0; i < live; ++i)
        {
            if (s_suppressed_hashes[i].load(std::memory_order_relaxed) == part_hash)
            {
                at = i;
                break;
            }
        }

        if (suppressed)
        {
            if (at != MAX_SUPPRESSED || live >= MAX_SUPPRESSED)
                return;
            // Publish the entry before the count that exposes it, so a scan never reads an uninitialized slot.
            s_suppressed_hashes[live].store(part_hash, std::memory_order_relaxed);
            s_suppressed_count.store(live + 1, std::memory_order_release);
            return;
        }

        if (at == MAX_SUPPRESSED)
            return;
        // Retire the count first, then backfill the hole from the tail. The other order exposes the tail entry twice
        // to a concurrent scan.
        s_suppressed_count.store(live - 1, std::memory_order_release);
        if (at != live - 1)
        {
            s_suppressed_hashes[at].store(
                s_suppressed_hashes[live - 1].load(std::memory_order_relaxed),
                std::memory_order_relaxed
            );
        }
    }

    void clear_all_suppressed() noexcept
    {
        s_suppressed_count.store(0, std::memory_order_release);
    }

    void set_mask(std::uint32_t category_mask) noexcept
    {
        clear_all_suppressed();
        if (!s_slot_hashes_ready.load(std::memory_order_acquire))
            return;

        for (const auto &entry : s_slot_hashes)
        {
            if ((category_mask & (std::uint32_t{1} << entry.slot)) == 0)
                continue;
            for (std::uint32_t h : entry.hashes)
            {
                if (h != 0)
                    set_hash_suppressed(h, true);
            }
        }
    }

    std::size_t init_slot_hashes(const std::unordered_map<std::string, std::uint32_t> &name_to_hash) noexcept
    {
        auto &logger = DMK::log();
        std::size_t resolved = 0;

        // Walk slot_metadata; only rows where part_show_hash_key is set (the 5 armor slots) participate in
        // IndexedStringA-driven suppression. Other rows (accessories, weapons) keep their zero-initialized
        // s_slot_hashes entry as a no-op so set_mask calls for those bits silently pass through.
        for (std::size_t i = 0; i < SLOT_COUNT; ++i)
        {
            s_slot_hashes[i].slot = i;
            s_slot_hashes[i].hashes = {0, 0, 0, 0};

            const char *part_show_key = SLOT_METADATA[i].part_show_hash_key;
            if (!part_show_key || part_show_key[0] == '\0')
                continue;

            auto it = name_to_hash.find(part_show_key);
            if (it == name_to_hash.end())
            {
                logger.warning(
                    "[dispatch] slot hash missing: '{}' not found in IndexedStringA - suppression for slot {} disabled",
                    part_show_key,
                    i
                );
                continue;
            }

            s_slot_hashes[i].hashes[0] = it->second;
            logger.info("[dispatch] slot hash resolved: {} = 0x{:X}", part_show_key, it->second);
            ++resolved;
        }

        s_slot_hashes_ready.store(resolved > 0, std::memory_order_release);
        return resolved;
    }

    bool slot_hashes_ready() noexcept
    {
        return s_slot_hashes_ready.load(std::memory_order_acquire);
    }

    __int64 __fastcall on_part_add_show(
        __int64 a1,
        std::uint8_t a2,
        std::uint64_t part_hash_ptr,
        float blend,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8,
        __int64 a9
    )
    {
        // Snapshot the trampoline pointer at entry. SafetyHook drains in-flight callers under its own shared lock, but
        // the brief teardown window between hook removal and the loader unmapping the Logic DLL can still see this body
        // executing with a torn trampoline if a game thread re-enters after the drain. A null snapshot means teardown
        // is in progress; bailing to 0 (the suppressed-return convention) mirrors what an early-cancelled call would
        // have produced.
        const auto trampoline = s_original_part_add_show.load(std::memory_order_acquire);
        if (!trampoline)
            return 0;

        const std::uint32_t part_hash = read_hash_safe(part_hash_ptr);
        const std::uint32_t idx = part_hash & 0xFFFF;
        const bool enabled = flag_enabled().load(std::memory_order_relaxed);
        const bool suppress = enabled && part_hash != 0 && hash_suppressed(part_hash);

        if (part_hash != 0 && s_hash_seen[idx].exchange(1, std::memory_order_relaxed) == 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Trace,
                "[dispatch] PartAddShow unique hash=0x{:08X} low16=0x{:04X} a2={:#04x} enabled={} suppress={}",
                part_hash,
                idx,
                static_cast<unsigned>(a2),
                enabled,
                suppress
            );
        }

        if (suppress)
        {
            // Per-suppress diagnostic kept at trace level - this fires on the hot PartAddShow dispatch path, so INFO
            // would flood the log. try_log, not trace(): the detour runs inside an engine frame, so a formatting or
            // sink failure must not escape it.
            (void)DMK::log()
                .try_log(DMK::LogLevel::Trace, "[dispatch] PartAddShow suppressed hash=0x{:08X}", part_hash);
            return 0;
        }

        return trampoline(a1, a2, part_hash_ptr, blend, a5, a6, a7, a8, a9);
    }

} // namespace Transmog::part_show_suppress
