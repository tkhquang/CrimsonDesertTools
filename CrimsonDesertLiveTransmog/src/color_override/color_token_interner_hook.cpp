#include "color_token_interner_hook.hpp"

#include "../aob_resolver.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace DMK = DetourModKit;

namespace Transmog::ColorOverride::InternerHook
{
    namespace
    {
        // Captured (name, token) pairs. Names are stable
        // process-lifetime pointers (engine's heap-allocated string
        // copies referenced by the interner's entries array).
        struct Entry
        {
            const char *name;
            std::uint32_t token;
        };
        // Engine's interner sentinel-cap is 0x2FFFF (196607).
        // Size our capture table to absorb realistic full dumps
        // (live tables typically run 10k-30k entries on a loaded
        // scene; the engine reserves up to ~200k slots).
        constexpr std::size_t k_cap = 65536;
        std::array<std::atomic<const char *>, k_cap> g_names{};
        std::array<std::atomic<std::uint32_t>, k_cap> g_tokens{};
        std::atomic<std::size_t> g_count{0};
        std::once_flag g_initOnce;
        std::atomic<bool> g_dumped{false};

        // Cached interner-state pointer location, captured at init()
        // success. `*g_stateSlot` is the live state pointer; the
        // engine may reallocate the hash-table backing arrays when
        // entries grow, so refresh() reads through this each call.
        std::atomic<std::uintptr_t> g_stateSlot{0};

        // Field offset (within the state struct) that holds the
        // entries-array pointer. The engine has shifted this between
        // +0x40 and +0x48 across patches; init() probes both and
        // caches whichever address actually contains valid records
        // here so refresh() reads through the same offset.
        std::atomic<std::ptrdiff_t> g_offEntriesArray{0x48};

        // Incremental-walk cursor used by refresh(). When the engine
        // reallocates the entries array, `g_lastEntriesBase` differs
        // from the live base and we restart from index 0; otherwise
        // we resume from `g_lastEntriesIdx` and only capture entries
        // appended since the previous walk. Without this, every
        // refresh re-walked the whole array and appended the engine's
        // already-captured entries as duplicates, exhausting the
        // capture cap within a few seconds on a populated scene.
        std::atomic<std::uintptr_t> g_lastEntriesBase{0};
        std::atomic<std::size_t>    g_lastEntriesIdx{0};

        bool safe_is_property_name(const char *p) noexcept
        {
            if (p == nullptr) return false;
            __try
            {
                if (p[0] != '_') return false;
                for (std::size_t i = 1; i < 80; ++i)
                {
                    const char c = p[i];
                    if (c == '\0') return i >= 2;
                    const auto uc =
                        static_cast<unsigned char>(c);
                    if (uc < 0x20 || uc > 0x7E) return false;
                }
                return false;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Scan the function body for the FIRST
        // `mov [rip+disp32], REG` whose target slot lies inside the
        // loaded module AND whose runtime value already looks like a
        // valid heap pointer. That write is the engine's
        // `qword_145E15620 = state` publish (in v1.06: encoded as
        // `48 89 35 disp32` -- `mov [rip+d], rsi` -- at function
        // offset 0x312).
        //
        // REX byte: 0x48 covers rax-rdi, 0x4C covers r8-r15.
        // ModR/M:   `(byte & 0xC7) == 0x05` selects mod=00 / r/m=101
        //           (RIP-relative addressing). The middle 3 bits hold
        //           the source register and are wildcarded by the
        //           mask so the scan tolerates compiler register-
        //           allocation churn between patches.
        //
        // Filtering on "slot's current value is a heap pointer"
        // discards early-init writes to other module globals (zeros,
        // small ints, sentinels) that share the encoding.
        std::uintptr_t find_state_slot_in_function(
            std::uintptr_t funcAddr,
            std::uintptr_t modBase,
            std::size_t modSize) noexcept
        {
            __try
            {
                const auto *bytes =
                    reinterpret_cast<const std::uint8_t *>(funcAddr);
                constexpr std::size_t k_scanLen = 0x500;
                for (std::size_t i = 0;
                     i + 7 <= k_scanLen; ++i)
                {
                    const auto b0 = bytes[i];
                    const auto b1 = bytes[i + 1];
                    const auto b2 = bytes[i + 2];
                    if ((b0 == 0x48 || b0 == 0x4C) &&
                        b1 == 0x89 &&
                        (b2 & 0xC7) == 0x05)
                    {
                        const std::int32_t disp =
                            *reinterpret_cast<const std::int32_t *>(
                                bytes + i + 3);
                        const auto target =
                            funcAddr + i + 7 +
                            static_cast<std::intptr_t>(disp);
                        if (target < modBase ||
                            target >= modBase + modSize)
                            continue;
                        // Check the slot's value: must be a heap
                        // pointer (init has run by the time we
                        // scan). This filters out non-state writes
                        // (e.g. early-init writes to other globals
                        // before the state is allocated).
                        const auto v = *reinterpret_cast<
                            const std::uint64_t *>(target);
                        if (v >= 0x10000ULL &&
                            v < 0x7FFFFFFFFFFFULL)
                            return target;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return 0;
        }

        // Walk the interner's entries array and capture every
        // (name, token) pair from `startIdx` onward. Returns the
        // number of entries captured into the global table; on
        // return `nextIdx` is set to the array index where the walk
        // stopped (caller persists this so subsequent walks can
        // resume incrementally instead of re-appending the engine's
        // already-captured prefix as duplicates).
        std::size_t walk_entries_array(
            std::uintptr_t entriesBase,
            std::size_t startIdx,
            std::size_t &nextIdx) noexcept
        {
            constexpr std::size_t k_maxWalk = 0x20000; // 128k entries
            constexpr std::ptrdiff_t k_entryStride = 32;
            constexpr std::ptrdiff_t k_offName = 0x08;
            constexpr std::ptrdiff_t k_offToken = 0x18;
            std::size_t captured = 0;
            std::size_t consecutiveBad = 0;
            std::size_t i = startIdx;
            for (; i < k_maxWalk; ++i)
            {
                const auto entryAddr =
                    entriesBase + i * k_entryStride;
                const auto namePtr = DMKMemory::seh_read<std::uint64_t>(
                                         entryAddr + k_offName)
                                         .value_or(0);
                const auto token = DMKMemory::seh_read<std::uint32_t>(
                                       entryAddr + k_offToken)
                                       .value_or(0);
                // Bail when we walk off the end -- consecutive
                // entries with null/garbage indicate we've left the
                // allocated array.
                if (namePtr < 0x10000ULL ||
                    namePtr > 0x7FFFFFFFFFFFULL ||
                    token == 0 || token > 0x100000u)
                {
                    if (++consecutiveBad >= 16) break;
                    continue;
                }
                consecutiveBad = 0;
                const auto *name =
                    reinterpret_cast<const char *>(namePtr);
                if (!safe_is_property_name(name)) continue;
                const auto idx = g_count.fetch_add(
                    1, std::memory_order_acq_rel);
                if (idx >= k_cap)
                {
                    g_count.fetch_sub(
                        1, std::memory_order_acq_rel);
                    break;
                }
                g_names[idx].store(name,
                                   std::memory_order_release);
                g_tokens[idx].store(token,
                                    std::memory_order_release);
                ++captured;
            }
            // Walk back over the trailing run of bad entries so a
            // future grow that fills the gap is detected next time.
            // `consecutiveBad` is the number of consecutive misses
            // we saw before bailing; the first "real" entry sits at
            // `i - consecutiveBad`, so resume at that index.
            nextIdx = (i > consecutiveBad) ? (i - consecutiveBad) : i;
            return captured;
        }

        void do_init() noexcept
        {
            auto &logger = DMK::Logger::get_instance();
            const auto range = DMKMemory::host_module_range();
            if (!range.valid()) return;
            const auto modBase = range.base;
            const auto modSize =
                static_cast<std::size_t>(range.end - range.base);

            const auto funcAddr = Transmog::resolve_address(
                Transmog::k_colorTokenInternerCandidates,
                "ColorTokenInterner");
            if (funcAddr == 0)
            {
                logger.warning(
                    "[interner-hook] sub_140F46680 not found");
                return;
            }
            const auto stateSlot = find_state_slot_in_function(
                funcAddr, modBase, modSize);
            if (stateSlot == 0)
            {
                logger.warning(
                    "[interner-hook] qword_145E15620 not found in "
                    "interner body (func=0x{:X})",
                    funcAddr);
                return;
            }
            const auto stateAddr = DMKMemory::seh_read<std::uint64_t>(stateSlot).value_or(0);
            if (stateAddr < 0x10000ULL)
            {
                logger.warning(
                    "[interner-hook] state pointer at 0x{:X} not "
                    "yet initialised (will retry lazily)",
                    stateSlot);
                return;
            }
            // State-struct field layout (decompile of sub_140F46680):
            //   state + 0x30 = num_buckets       (u32)
            //   state + 0x40 = bucket_array_ptr  (u64)
            //   state + 0x48 = entries_array_ptr (u64)
            // Live capture in v1.06 has shown the entries-array
            // pointer landing at +0x40 in some builds and +0x48 in
            // others (the engine appears to have shifted the field
            // by one slot during a header rev). Probe both with a
            // small leading-record validator and persist the chosen
            // offset in g_offEntriesArray so refresh() reads through
            // the same field on subsequent walks.
            auto probe = [](std::uintptr_t base) noexcept -> std::size_t {
                if (base < 0x10000ULL) return 0;
                std::size_t valid = 0;
                for (std::size_t i = 0; i < 256; ++i)
                {
                    const auto e = base + i * 32;
                    const auto np = DMKMemory::seh_read<std::uint64_t>(e + 0x08).value_or(0);
                    const auto tk = DMKMemory::seh_read<std::uint32_t>(e + 0x18).value_or(0);
                    if (np < 0x10000ULL || np > 0x7FFFFFFFFFFFULL)
                        continue;
                    if (tk == 0 || tk > 0x100000u) continue;
                    if (safe_is_property_name(
                            reinterpret_cast<const char *>(np)))
                        ++valid;
                }
                return valid;
            };
            const auto base40 = DMKMemory::seh_read<std::uint64_t>(stateAddr + 0x40).value_or(0);
            const auto base48 = DMKMemory::seh_read<std::uint64_t>(stateAddr + 0x48).value_or(0);
            const auto v40 = probe(base40);
            const auto v48 = probe(base48);
            logger.info(
                "[interner-hook] probe state=0x{:X} "
                "base40=0x{:X} valid40={} base48=0x{:X} valid48={}",
                stateAddr, base40, v40, base48, v48);
            const bool pick40 = (v40 >= v48 && base40 >= 0x10000ULL);
            const auto entriesBase = pick40 ? base40 : base48;
            const std::ptrdiff_t entriesOff = pick40 ? 0x40 : 0x48;
            if (entriesBase < 0x10000ULL)
            {
                logger.warning(
                    "[interner-hook] neither state+0x40 nor +0x48 "
                    "has a valid entries-array pointer");
                return;
            }
            std::size_t nextIdx = 0;
            const auto captured = walk_entries_array(
                entriesBase, 0, nextIdx);
            logger.info(
                "[interner-hook] func=0x{:X} stateSlot=0x{:X} "
                "state=0x{:X} entries=0x{:X} off=0x{:X} captured={} "
                "nextIdx={}",
                funcAddr, stateSlot, stateAddr, entriesBase,
                entriesOff, captured, nextIdx);
            g_offEntriesArray.store(entriesOff,
                                    std::memory_order_release);
            g_lastEntriesBase.store(entriesBase,
                                    std::memory_order_release);
            g_lastEntriesIdx.store(nextIdx,
                                   std::memory_order_release);
            g_stateSlot.store(stateSlot, std::memory_order_release);
            g_dumped.store(true, std::memory_order_release);
        }
    } // namespace

    bool init() noexcept
    {
        std::call_once(g_initOnce, []() { do_init(); });
        return g_dumped.load(std::memory_order_acquire);
    }

    std::size_t refresh() noexcept
    {
        // No hard attempt cap. Mirroring item_name_table's stability
        // check: keep walking the engine's interner each time the
        // setter sees an unclassified token, and only stop once two
        // consecutive walks return the same g_count -- that means
        // the engine has stopped interning new names. After settle
        // this becomes a permanent no-op for the rest of the session.
        //
        // Throttled to ~1.5 s between walks because each walk is
        // O(entries); the engine can have 100k+ entries in a loaded
        // scene.
        static std::atomic<bool> s_settled{false};
        static std::atomic<long long> s_lastMs{0};
        static std::atomic<bool> s_busy{false};
        static std::atomic<std::size_t> s_lastCount{0};
        if (s_settled.load(std::memory_order_acquire)) return 0;
        using clock = std::chrono::steady_clock;
        const auto nowMs = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                clock::now().time_since_epoch()).count();
        auto last = s_lastMs.load(std::memory_order_acquire);
        if (last != 0 && (nowMs - last) < 1500) return 0;
        const auto slot = g_stateSlot.load(std::memory_order_acquire);
        if (slot == 0) return 0;
        bool expected = false;
        if (!s_busy.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return 0;
        s_lastMs.store(nowMs, std::memory_order_release);
        const auto stateAddr = DMKMemory::seh_read<std::uint64_t>(slot).value_or(0);
        std::size_t added = 0;
        if (stateAddr >= 0x10000ULL)
        {
            const auto entriesOff = g_offEntriesArray.load(
                std::memory_order_acquire);
            const auto entriesBase = DMKMemory::seh_read<std::uint64_t>(
                                         stateAddr + entriesOff)
                                         .value_or(0);
            if (entriesBase >= 0x10000ULL)
            {
                // Resume the walk where the previous one stopped if
                // the engine hasn't reallocated the entries array;
                // otherwise restart from index 0 against the new
                // base so we capture every entry exactly once.
                const auto cachedBase = g_lastEntriesBase.load(
                    std::memory_order_acquire);
                std::size_t startIdx = 0;
                if (cachedBase == entriesBase)
                    startIdx = g_lastEntriesIdx.load(
                        std::memory_order_acquire);
                const auto before = g_count.load(
                    std::memory_order_acquire);
                std::size_t nextIdx = startIdx;
                walk_entries_array(entriesBase, startIdx, nextIdx);
                const auto after = g_count.load(
                    std::memory_order_acquire);
                added = (after > before) ? (after - before) : 0;
                g_lastEntriesBase.store(entriesBase,
                                        std::memory_order_release);
                g_lastEntriesIdx.store(nextIdx,
                                       std::memory_order_release);
                const auto prev = s_lastCount.exchange(
                    after, std::memory_order_acq_rel);
                if (added > 0)
                {
                    DMK::Logger::get_instance().info(
                        "[interner-hook] refresh: entries=0x{:X} "
                        "off=0x{:X} resumeFrom={} added={} total={} "
                        "prev_total={} nextIdx={}",
                        entriesBase, entriesOff, startIdx, added,
                        after, prev, nextIdx);
                }
                // Two consecutive walks with the same total = the
                // interner has stopped growing. Require a non-zero
                // baseline so an early empty-table walk can't latch
                // settled immediately.
                if (after == prev && after > 0)
                {
                    s_settled.store(true, std::memory_order_release);
                    DMK::Logger::get_instance().info(
                        "[interner-hook] settled at {} captures; "
                        "no further refreshes this session", after);
                }
            }
        }
        s_busy.store(false, std::memory_order_release);
        return added;
    }

    const char *name_for_token(std::uint32_t tok) noexcept
    {
        if (tok == 0) return nullptr;
        const auto cnt = g_count.load(std::memory_order_acquire);
        const auto upper = (cnt < k_cap) ? cnt : k_cap;
        for (std::size_t i = 0; i < upper; ++i)
        {
            if (g_tokens[i].load(std::memory_order_relaxed) == tok)
                return g_names[i].load(std::memory_order_relaxed);
        }
        return nullptr;
    }

    std::uint32_t token_for_name(const char *name) noexcept
    {
        if (name == nullptr) return 0;
        const auto cnt = g_count.load(std::memory_order_acquire);
        const auto upper = (cnt < k_cap) ? cnt : k_cap;
        for (std::size_t i = 0; i < upper; ++i)
        {
            const auto p = g_names[i].load(
                std::memory_order_relaxed);
            if (p != nullptr && std::strcmp(p, name) == 0)
                return g_tokens[i].load(
                    std::memory_order_relaxed);
        }
        return 0;
    }

    std::size_t capture_count() noexcept
    {
        return g_count.load(std::memory_order_acquire);
    }
}
