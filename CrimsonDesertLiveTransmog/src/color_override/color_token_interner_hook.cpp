#include "color_token_interner_hook.hpp"

#include "../aob_resolver.hpp"
#include "color_state.hpp"

#include <DetourModKit.hpp>
#include <DetourModKit/memory.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace Transmog::color_override::interner_hook
{
    namespace
    {
        // Captured (name, token) pairs. Names are stable process-lifetime pointers (engine's heap-allocated string
        // copies referenced by the interner's entries array).
        struct Entry
        {
            const char *name{};
            std::uint32_t token{};
        };
        // Engine's interner sentinel-cap is 0x2FFFF (196607). Size our capture table to absorb realistic full dumps
        // (live tables typically run 10k-30k entries on a loaded scene; the engine reserves up to ~200k slots).
        constexpr std::size_t CAP = 65536;
        std::array<std::atomic<const char *>, CAP> g_names{};
        std::array<std::atomic<std::uint32_t>, CAP> g_tokens{};
        std::atomic<std::size_t> g_count{0};
        std::atomic<bool> g_dumped{false};
        // Serializes init() attempts (do_init mutates the shared capture table) and gates the one-shot "state not yet
        // published" notice so lazy retries don't spam the log.
        std::atomic<bool> g_captureBusy{false};
        std::atomic<bool> g_loggedPending{false};

        // Cached interner-state pointer location, captured at init() success. `*g_stateSlot` is the live state pointer;
        // the engine may reallocate the hash-table backing arrays when entries grow, so refresh() reads through this
        // each call.
        std::atomic<std::uintptr_t> g_stateSlot{0};

        // Field offset inside the state struct that holds the entries-array pointer. The engine shifted this between
        // +0x40 and +0x48 across patches. init() probes both and caches whichever address holds valid records, so
        // refresh() reads through the same offset.
        std::atomic<std::ptrdiff_t> g_offEntriesArray{0x48};

        // Incremental-walk cursor used by refresh(). When the engine reallocates the entries array, `g_lastEntriesBase`
        // differs from the live base and we restart from index 0; otherwise we resume from `g_lastEntriesIdx` and only
        // capture entries appended since the previous walk. Without this, every refresh re-walked the whole array and
        // appended the engine's already-captured entries as duplicates, exhausting the capture cap within a few seconds
        // on a populated scene.
        std::atomic<std::uintptr_t> g_lastEntriesBase{0};
        std::atomic<std::size_t> g_lastEntriesIdx{0};

        bool safe_is_property_name(const char *p) noexcept
        {
            if (p == nullptr)
                return false;
            __try
            {
                if (p[0] != '_')
                    return false;
                for (std::size_t i = 1; i < 80; ++i)
                {
                    const char c = p[i];
                    if (c == '\0')
                        return i >= 2;
                    const auto uc = static_cast<unsigned char>(c);
                    if (uc < 0x20 || uc > 0x7E)
                        return false;
                }
                return false;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Scan the function body for the FIRST `mov [rip+disp32], REG` whose target slot lies inside the loaded
        // module. That write is the engine `qword = state` publish, encoded as `48 89 35 disp32`, a `mov [rip+d], rsi`.
        // The slot ADDRESS drifts across patches, so it is always computed from the RIP displacement and never
        // hardcoded.
        //
        // REX byte: 0x48 covers rax-rdi, 0x4C covers r8-r15.
        // ModR/M: `(byte & 0xC7) == 0x05` selects mod=00 and r/m=101, which is RIP-relative addressing. The middle 3
        //         bits hold the source register and the mask wildcards them, so the scan tolerates compiler
        //         register-allocation churn between patches.
        //
        // Value-based disambiguation: a slot already holding a heap pointer is a confident match, because the engine
        // published the state. The engine fills that global LAZILY on first interner use, which happens AFTER mod
        // startup, and until then the slot reads 0. The publish is structurally the first in-module RIP-relative qword
        // store in this function, so the scan prefers a heap pointer and falls back to the first ZEROED store. That
        // resolves the slot both before and after the engine populates it. A non-zero, non-heap value such as a small
        // int or a sentinel still rejects, which preserves the guard against unrelated early-init globals that share
        // the encoding.
        std::uintptr_t
        find_state_slot_in_function(std::uintptr_t func_addr, std::uintptr_t mod_base, std::size_t mod_size) noexcept
        {
            __try
            {
                const auto *bytes = reinterpret_cast<const std::uint8_t *>(func_addr);
                constexpr std::size_t scan_len = 0x500;
                std::uintptr_t first_zero_slot = 0;
                for (std::size_t i = 0; i + 7 <= scan_len; ++i)
                {
                    const auto b0 = bytes[i];
                    const auto b1 = bytes[i + 1];
                    const auto b2 = bytes[i + 2];
                    if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x89 && (b2 & 0xC7) == 0x05)
                    {
                        const std::int32_t disp = *reinterpret_cast<const std::int32_t *>(bytes + i + 3);
                        const auto target = func_addr + i + 7 + static_cast<std::intptr_t>(disp);
                        if (target < mod_base || target >= mod_base + mod_size)
                            continue;
                        // Heap pointer -> confident, return now.
                        // Exactly zero -> pre-publish slot; remember
                        // the FIRST as a fallback. Otherwise skip (unrelated early-init global).
                        const auto v = *reinterpret_cast<const std::uint64_t *>(target);
                        if (DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(v)}))
                            return target;
                        if (v == 0 && first_zero_slot == 0)
                            first_zero_slot = target;
                    }
                }
                return first_zero_slot;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return 0;
        }

        // Walk the interner's entries array and capture every (name, token) pair from `start_idx` onward. Returns the
        // number of entries captured into the global table; on return `next_idx` is set to the array index where the
        // walk stopped (caller persists this so subsequent walks can resume incrementally instead of re-appending the
        // engine's already-captured prefix as duplicates).
        std::size_t
        walk_entries_array(std::uintptr_t entries_base, std::size_t start_idx, std::size_t &next_idx) noexcept
        {
            constexpr std::size_t max_walk = 0x20000; // 128k entries
            constexpr std::ptrdiff_t entry_stride = 32;
            constexpr std::ptrdiff_t off_name = 0x08;
            constexpr std::ptrdiff_t off_token = 0x18;
            std::size_t captured = 0;
            std::size_t consecutive_bad = 0;
            std::size_t i = start_idx;
            for (; i < max_walk; ++i)
            {
                const auto entry_addr = entries_base + i * entry_stride;
                const auto name_ptr = DMK::memory::read<std::uint64_t>(DMK::Address{entry_addr + off_name}).value_or(0);
                const auto token = DMK::memory::read<std::uint32_t>(DMK::Address{entry_addr + off_token}).value_or(0);
                // Bail when the walk runs off the end. Consecutive entries with null or garbage mean the walk left
                // the allocated array.
                if (!DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(name_ptr)}) || token == 0 ||
                    token > 0x100000u)
                {
                    if (++consecutive_bad >= 16)
                        break;
                    continue;
                }
                consecutive_bad = 0;
                const auto *name = reinterpret_cast<const char *>(name_ptr);
                if (!safe_is_property_name(name))
                    continue;
                const auto idx = g_count.fetch_add(1, std::memory_order_acq_rel);
                if (idx >= CAP)
                {
                    g_count.fetch_sub(1, std::memory_order_acq_rel);
                    break;
                }
                g_names[idx].store(name, std::memory_order_release);
                g_tokens[idx].store(token, std::memory_order_release);
                ++captured;
            }
            // Walk back over the trailing run of bad entries so a future grow that fills the gap is detected next time.
            // `consecutive_bad` is the number of consecutive misses we saw before bailing; the first "real" entry sits
            // at `i - consecutive_bad`, so resume at that index.
            next_idx = (i > consecutive_bad) ? (i - consecutive_bad) : i;
            return captured;
        }

        void do_init() noexcept
        {
            auto &logger = DMK::log();

            // Stage 1: resolve the interner function and the state-slot ADDRESS. Timing-independent - it locates
            // the publish *instruction*, not a populated value - so it succeeds even at startup, before the engine has
            // run the interner's once-only init path. Runs once; the resolved slot is cached in g_stateSlot and reused
            // on every retry.
            auto state_slot = g_stateSlot.load(std::memory_order_acquire);
            if (state_slot == 0)
            {
                const auto range = DMK::Region::host();
                if (range.size == 0)
                    return;
                const auto mod_base = range.base.raw();
                const auto mod_size = range.size;
                const auto func_addr = anchor_address(AnchorId::ColorTokenInterner);
                if (func_addr == 0)
                {
                    logger.warning("[interner-hook] ColorTokenInterner not resolved");
                    return;
                }
                state_slot = find_state_slot_in_function(func_addr, mod_base, mod_size);
                if (state_slot == 0)
                {
                    logger.warning(
                        "[interner-hook] state-publish store not found in interner body (func=0x{:X})",
                        func_addr
                    );
                    return;
                }
                g_stateSlot.store(state_slot, std::memory_order_release);
                logger.info("[interner-hook] resolved state slot 0x{:X} (func=0x{:X})", state_slot, func_addr);
            }

            // Stage 2: read the published state pointer and walk the entries array. Retry-able - the engine
            // publishes the state lazily on first shader-property registration, which can occur after mod startup.
            // While the slot still reads 0 we leave g_dumped false and return; refresh() re-drives this until the slot
            // is populated.
            const auto state_addr = DMK::memory::read<std::uint64_t>(DMK::Address{state_slot}).value_or(0);
            if (!DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(state_addr)}))
            {
                if (!g_loggedPending.exchange(true, std::memory_order_acq_rel))
                    logger.info(
                        "[interner-hook] state slot 0x{:X} not yet "
                        "published; will capture lazily on first interner use",
                        state_slot
                    );
                return;
            }
            // state-struct field layout, from the interner body:
            //   state + 0x30 = num_buckets       (u32)
            //   state + 0x40 = bucket_array_ptr  (u64)
            //   state + 0x48 = entries_array_ptr (u64)
            // Live capture puts the entries-array pointer at +0x40 in some builds and +0x48 in others, because the
            // engine shifted the field by one slot during a header rev. probe both with a small leading-record
            // validator and persist the chosen offset in g_offEntriesArray so refresh() reads through the same field
            // on later walks.
            auto probe = [](std::uintptr_t base) noexcept -> std::size_t
            {
                if (!DMK::memory::is_plausible_ptr(DMK::Address{base}))
                    return 0;
                std::size_t valid = 0;
                for (std::size_t i = 0; i < 256; ++i)
                {
                    const auto e = base + i * 32;
                    const auto np = DMK::memory::read<std::uint64_t>(DMK::Address{e + 0x08}).value_or(0);
                    const auto tk = DMK::memory::read<std::uint32_t>(DMK::Address{e + 0x18}).value_or(0);
                    if (!DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(np)}))
                        continue;
                    if (tk == 0 || tk > 0x100000u)
                        continue;
                    if (safe_is_property_name(reinterpret_cast<const char *>(np)))
                        ++valid;
                }
                return valid;
            };
            const auto base40 = DMK::memory::read<std::uint64_t>(DMK::Address{state_addr + 0x40}).value_or(0);
            const auto base48 = DMK::memory::read<std::uint64_t>(DMK::Address{state_addr + 0x48}).value_or(0);
            const auto v40 = probe(base40);
            const auto v48 = probe(base48);
            logger.info(
                "[interner-hook] probe state=0x{:X} base40=0x{:X} valid40={} base48=0x{:X} valid48={}",
                state_addr,
                base40,
                v40,
                base48,
                v48
            );
            const bool pick40 =
                (v40 >= v48 && DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(base40)}));
            const auto entries_base = pick40 ? base40 : base48;
            const std::ptrdiff_t entries_off = pick40 ? 0x40 : 0x48;
            if (!DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(entries_base)}))
            {
                logger.warning("[interner-hook] neither state+0x40 nor +0x48 has a valid entries-array pointer");
                return;
            }
            std::size_t next_idx = 0;
            const auto captured = walk_entries_array(entries_base, 0, next_idx);
            logger.info(
                "[interner-hook] stateSlot=0x{:X} state=0x{:X} entries=0x{:X} off=0x{:X} captured={} nextIdx={}",
                state_slot,
                state_addr,
                entries_base,
                entries_off,
                captured,
                next_idx
            );
            g_offEntriesArray.store(entries_off, std::memory_order_release);
            g_lastEntriesBase.store(entries_base, std::memory_order_release);
            g_lastEntriesIdx.store(next_idx, std::memory_order_release);
            g_dumped.store(true, std::memory_order_release);
        }
    } // namespace

    bool init() noexcept
    {
        if (g_dumped.load(std::memory_order_acquire))
            return true;
        // Re-entrant: a single early attempt can find the interner's state global still unpublished (the engine fills
        // it lazily on first use, which may be after mod startup). Serialize attempts - do_init mutates the shared
        // capture table - and latch g_dumped only on a successful walk. refresh() drives the retries until then.
        bool expected = false;
        if (!g_captureBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return g_dumped.load(std::memory_order_acquire);
        do_init();
        g_captureBusy.store(false, std::memory_order_release);
        return g_dumped.load(std::memory_order_acquire);
    }

    std::size_t refresh() noexcept
    {
        // No hard attempt cap. Mirroring item_name_table's stability check: keep walking the engine's interner each
        // time the setter sees an unclassified token, and only stop once two consecutive walks return the same g_count
        // - that means the engine has stopped interning new names. After settle this becomes a permanent no-op for the
        // rest of the session.
        //
        // Throttled to ~1.5 s between walks because each walk is
        // O(entries); the engine can have 100k+ entries in a loaded scene.
        static std::atomic<bool> s_settled{false};
        static std::atomic<std::int64_t> s_last_ms{0};
        static std::atomic<bool> s_busy{false};
        static std::atomic<std::size_t> s_last_count{0};
        if (s_settled.load(std::memory_order_acquire))
            return 0;
        // One clock owner for the whole module, so this throttle and the apply window share a time base by
        // construction.
        const auto now = state::now_ms();
        const auto last = s_last_ms.load(std::memory_order_acquire);
        if (last != 0 && (now - last) < 1500)
            return 0;
        // Until the first successful capture, re-drive init(): the interner's state global may not have been published
        // when init() ran at startup (the engine fills it lazily on first shader-property registration). do_init()
        // latches g_dumped and performs the initial walk once the slot is populated. Throttled to the same 1.5s cadence
        // as the walk below.
        if (!g_dumped.load(std::memory_order_acquire))
        {
            s_last_ms.store(now, std::memory_order_release);
            init();
            return 0;
        }
        const auto slot = g_stateSlot.load(std::memory_order_acquire);
        if (slot == 0)
            return 0;
        bool expected = false;
        if (!s_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return 0;
        s_last_ms.store(now, std::memory_order_release);
        const auto state_addr = DMK::memory::read<std::uint64_t>(DMK::Address{slot}).value_or(0);
        std::size_t added = 0;
        if (DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(state_addr)}))
        {
            const auto entries_off = g_offEntriesArray.load(std::memory_order_acquire);
            const auto entries_base =
                DMK::memory::read<std::uint64_t>(DMK::Address{state_addr + entries_off}).value_or(0);
            if (DMK::memory::is_plausible_ptr(DMK::Address{static_cast<std::uintptr_t>(entries_base)}))
            {
                // Resume the walk where the previous one stopped if the engine has not reallocated the entries array;
                // otherwise restart from index 0 against the new base so we capture every entry exactly once.
                const auto cached_base = g_lastEntriesBase.load(std::memory_order_acquire);
                std::size_t start_idx = 0;
                if (cached_base == entries_base)
                    start_idx = g_lastEntriesIdx.load(std::memory_order_acquire);
                const auto before = g_count.load(std::memory_order_acquire);
                std::size_t next_idx = start_idx;
                walk_entries_array(entries_base, start_idx, next_idx);
                const auto after = g_count.load(std::memory_order_acquire);
                added = (after > before) ? (after - before) : 0;
                g_lastEntriesBase.store(entries_base, std::memory_order_release);
                g_lastEntriesIdx.store(next_idx, std::memory_order_release);
                const auto prev = s_last_count.exchange(after, std::memory_order_acq_rel);
                if (added > 0)
                {
                    DMK::log().info(
                        "[interner-hook] refresh: entries=0x{:X} "
                        "off=0x{:X} resumeFrom={} added={} total={} prev_total={} nextIdx={}",
                        entries_base,
                        entries_off,
                        start_idx,
                        added,
                        after,
                        prev,
                        next_idx
                    );
                }
                // Two consecutive walks with the same total = the interner has stopped growing. Require a non-zero
                // baseline so an early empty-table walk cannot latch settled immediately.
                if (after == prev && after > 0)
                {
                    s_settled.store(true, std::memory_order_release);
                    DMK::log().info("[interner-hook] settled at {} captures; no further refreshes this session", after);
                }
            }
        }
        s_busy.store(false, std::memory_order_release);
        return added;
    }

    const char *name_for_token(std::uint32_t tok) noexcept
    {
        if (tok == 0)
            return nullptr;
        const auto cnt = g_count.load(std::memory_order_acquire);
        const auto upper = (cnt < CAP) ? cnt : CAP;
        for (std::size_t i = 0; i < upper; ++i)
        {
            if (g_tokens[i].load(std::memory_order_relaxed) == tok)
                return g_names[i].load(std::memory_order_relaxed);
        }
        return nullptr;
    }

    std::uint32_t token_for_name(const char *name) noexcept
    {
        if (name == nullptr)
            return 0;
        const auto cnt = g_count.load(std::memory_order_acquire);
        const auto upper = (cnt < CAP) ? cnt : CAP;
        for (std::size_t i = 0; i < upper; ++i)
        {
            const auto p = g_names[i].load(std::memory_order_relaxed);
            if (p != nullptr && std::strcmp(p, name) == 0)
                return g_tokens[i].load(std::memory_order_relaxed);
        }
        return 0;
    }

    std::size_t capture_count() noexcept
    {
        return g_count.load(std::memory_order_acquire);
    }
} // namespace Transmog::color_override::interner_hook
