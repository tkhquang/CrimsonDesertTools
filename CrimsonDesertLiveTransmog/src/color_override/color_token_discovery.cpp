#include "color_token_discovery.hpp"

#include "../aob_resolver.hpp"
#include "color_state.hpp"

#include <DetourModKit.hpp>
#include <DetourModKit/memory.hpp>
#include <DetourModKit/region.hpp>
#include <DetourModKit/scan.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace Transmog::ColorOverride::TokenSlotDiscovery
{
    namespace
    {
        struct KnownProp
        {
            const char *name{};
            int layer{};
            int channel{};
        };
        constexpr KnownProp k_known[] = {
            {"_tintColorR", 0, 0},
            {"_tintColorG", 0, 1},
            {"_tintColorB", 0, 2},
            {"_dyeingColorMaskR", 1, 0},
            {"_dyeingColorMaskG", 1, 1},
            {"_dyeingColorMaskB", 1, 2},
            {"_dyeingDetailLayerColorMaskR", 2, 0},
            {"_dyeingDetailLayerColorMaskG", 2, 1},
            {"_dyeingDetailLayerColorMaskB", 2, 2},
            {"_hairDyeingColor", 3, -1},
            {"_dyeingCustomHighlightBitFlag", 1, -1},
            {"_dyeingCustomGrimeOpacityMaskR", 1, 0},
            {"_dyeingCustomGrimeOpacityMaskG", 1, 1},
            {"_dyeingCustomGrimeOpacityMaskB", 1, 2},
            {"_dyeingCustomPropertyMaskR", 1, 0},
            {"_dyeingCustomPropertyMaskG", 1, 1},
            {"_dyeingCustomPropertyMaskB", 1, 2},
            {"_dyeingGlobalOpacity", 1, -1},
        };

        struct DiscoveredSlot
        {
            std::uintptr_t slot_addr{};
            const char *name{};
            int layer{};
            int channel{};
        };
        std::vector<DiscoveredSlot> g_slots;

        // Guards every read and write of g_slots. The writer runs on the re-scan worker while the engine's property
        // setter reads through classify_layer / name_for_token on the game thread, so a push_back that reallocates
        // would otherwise invalidate an iterator another thread is walking. Held only around the dedup-and-append,
        // never across a sweep, so a reader waits microseconds rather than the length of a scan.
        std::mutex g_slotsMtx;
        std::atomic<bool> g_complete{false};

        // Re-scan worker state. The sweep is half a second of work, so it runs OFF the caller's thread: its only
        // caller is a mid-hook detour on the engine's per-channel property setter, and doing it inline stalled the
        // game thread for as long as the sweep took.
        std::mutex g_rescanThreadMtx;
        std::optional<DMK::StoppableWorker> g_rescanWorker;
        std::atomic<bool> g_rescanBusy{false};
        std::atomic<bool> g_stopping{false};
        std::atomic<bool> g_settled{false};
        std::atomic<std::size_t> g_lastCount{0};
        std::once_flag g_runOnce;

        // The longest entry in k_known is 30 characters, so a 32-byte window always contains the terminator and a
        // name longer than the window can never compare equal to an allow-list entry.
        constexpr std::size_t k_name_window = 32;

        // Copy a candidate property name out of the host image. One guarded read per hit replaces a per-known-name
        // walk over foreign memory, and the forced terminator makes std::strcmp over the copy the exact comparison
        // the allow-list needs.
        bool read_foreign_name(std::uintptr_t addr, char (&out)[k_name_window]) noexcept
        {
            out[0] = '\0';
            std::array<std::byte, k_name_window> raw{};
            // read_into fails the WHOLE span on a fault anywhere inside it, so a name that ends within the window of
            // an unmapped page would reject a candidate the byte-at-a-time predecessor accepted. Walk the window down
            // to its readable prefix and terminate there. A normal candidate reads in one pass.
            std::size_t readable = k_name_window;
            while (readable > 0 && !DMK::memory::read_into(DMK::Address{addr}, std::span{raw.data(), readable}))
                readable /= 2;
            if (readable == 0)
                return false;
            std::memcpy(out, raw.data(), readable);
            out[readable - 1] = '\0';
            return true;
        }

        // Read a printable preview of a candidate name for the diagnostic trace. `out` takes up to 32 printable
        // characters plus a terminator. A non-printable byte prints as '?'.
        bool peek_name_preview(std::uintptr_t addr, char *out, std::size_t out_cap) noexcept
        {
            if (out == nullptr || out_cap == 0)
                return false;
            out[0] = '\0';
            const std::size_t limit = (out_cap - 1 < k_name_window) ? out_cap - 1 : k_name_window;
            std::array<std::byte, k_name_window> raw{};
            if (!DMK::memory::read_into(DMK::Address{addr}, std::span{raw.data(), limit}))
            {
                std::snprintf(out, out_cap, "<fault>");
                return false;
            }
            std::size_t i = 0;
            for (; i < limit; ++i)
            {
                const auto uc = std::to_integer<unsigned char>(raw[i]);
                if (uc == 0)
                    break;
                out[i] = (uc >= 0x20 && uc <= 0x7E) ? static_cast<char>(uc) : '?';
            }
            out[i] = '\0';
            return true;
        }

        void do_run()
        {
            using clock = std::chrono::steady_clock;
            const auto t0 = clock::now();

            // Module base and image size, resolved once per sweep. Region::host() is loader-backed and re-walks the
            // PE headers on every call, so it stays out of the per-hit loop below.
            const auto range = DMK::Region::host();
            if (range.size == 0)
            {
                DMK::log().warning("[token-discovery] host image range unavailable");
                g_complete.store(true, std::memory_order_release);
                return;
            }

            // Walk every property-registration call site emitted by the two TLS-guarded property registrars. The
            // pattern matches the 17-byte run:
            //
            //   <zero-write>               6 or 7 bytes; decoded
            //                              backward to get slot addr
            //   41 B9 FF FF 02 00          mov  r9d, 0x2FFFF  (6B)
            //   ?? 8D ?? 01                lea  r8d, [reg+1]  (4B)
            //                              REX/ModR/M wildcarded
            //                              (reg is r13 for A, rdi
            //                              for B).
            //   48 8D 15 ?? ?? ?? ??       lea  rdx, [name]   (7B)
            //   <rcx load>                 3 or 7 bytes (lea or mov)
            //   E8 ?? ?? ?? ??             call interner      (5B)
            //
            // The `lea rdx` target is the property name string. The slot address is decoded from the zero-write
            // instruction immediately PRECEDING the `mov r9d, 0x2FFFF` anchor:
            //
            //   89 3D disp32         (6B) - mov [rip+d], edi
            //   44 89 2D disp32      (7B) - mov [rip+d], r13d
            //
            // Anchoring on the zero-write instead of `lea rcx` lets us catch the first-entry-per-function case where
            // rcx is loaded via `mov rcx, rbx` / `mov rcx, rsi` from a preloaded table-base register. That entry is
            // always `_dyeingColorMaskR` and matters because the live game emits its token id (0x2FCE).
            //
            // The pattern is intentionally non-unique: there is one match per registered property across the entire
            // module. The name-allow-list filter below accepts only the entries whose strings
            // appear in k_known, so over-scanning is a performance concern rather than a correctness one. Compile all 3
            // walk patterns up front. We try each in turn over the module range; the inner accept logic dedups by
            // slot_addr so a site matched by multiple patterns is recorded only once. Walking the union gives
            // resilience: if a future patch reshapes one pattern's tail, the others still cover the call site.
            // The candidates are compiled at BUILD time (Pattern::literal is consteval), so there is no parse step
            // that can fail at runtime and no compiled-count guard to write.
            const auto &patterns = Transmog::k_colorTokenRegistrarCallAobs;
            constexpr std::size_t compiledCount = Transmog::k_colorTokenRegistrarCallAobCount;

            // Zero-write decode. The instruction immediately preceding the matched `mov r9d, 0x2FFFF` anchor is one
            // of:
            //   `89 3D disp32`       (6B) - mov [rip+d], edi
            //   `44 89 2D disp32`    (7B) - mov [rip+d], r13d
            // Both encode the table slot address as RIP-relative disp32, resolved against the next-instruction RIP,
            // which is the anchor start. Each form is discriminated on its own guarded read of exactly the bytes that
            // form needs, so a 6-byte site one byte past a page boundary still decodes.
            auto decode_zero_write_slot = [](const std::byte *anchor) -> std::uintptr_t
            {
                // 6-byte form: `89 3D` at anchor-6
                std::array<std::byte, 2> head6{};
                if (DMK::memory::read_into(DMK::Address{anchor - 6}, std::span{head6}) &&
                    std::to_integer<std::uint8_t>(head6[0]) == 0x89 && std::to_integer<std::uint8_t>(head6[1]) == 0x3D)
                {
                    return DMK::scan::resolve_rip_relative(DMK::Address{anchor - 6}, 2, 6)
                        .value_or(DMK::Address{})
                        .raw();
                }
                // 7-byte form: `44 89 2D` at anchor-7
                std::array<std::byte, 3> head7{};
                if (DMK::memory::read_into(DMK::Address{anchor - 7}, std::span{head7}) &&
                    std::to_integer<std::uint8_t>(head7[0]) == 0x44 &&
                    std::to_integer<std::uint8_t>(head7[1]) == 0x89 && std::to_integer<std::uint8_t>(head7[2]) == 0x2D)
                {
                    return DMK::scan::resolve_rip_relative(DMK::Address{anchor - 7}, 3, 7)
                        .value_or(DMK::Address{})
                        .raw();
                }
                return 0;
            };

            // Linear walk through the module per pattern: for each compiled candidate, find_pattern returns the first
            // match in [cursor..end]; we advance past it and continue. The accept logic dedups by slot_addr so patterns
            // whose hit sets overlap (P1 is a superset of
            // P2 and P3) record each unique slot exactly once.
            //
            // The head pattern matches a large number of false positives early in the binary, because the wildcard
            // bytes give it slack and the real registrar sites sit late in the text section. The walk therefore has
            // to step past tens of thousands of them. The per-pattern cap is set generously; the shared wall-clock
            // budget across all patterns is the real safety net.
            // Sized well clear of the observed hit count (~45k on the live build, nearly all of them P1's false
            // positives) so ordinary drift between game builds cannot reach it. This is a runaway stop, not a tuning
            // knob: the wall-clock budget below is what actually bounds the sweep, and a cap sitting just above the
            // real count silently truncates the moment the game grows a few thousand more matching sites.
            constexpr std::size_t k_maxItersPerPattern = 250000;
            constexpr auto k_timeBudget = std::chrono::milliseconds(3000);
            // The anchor is the start of the `mov r9d, 0x2FFFF` sentinel. Advance the cursor by the shared head
            // length each iteration even when the matched pattern is longer (the P2/P3 tails are not needed for
            // overlap prevention; their hit ranges always extend past the head and never abut another distinct site).
            constexpr std::size_t k_patternHeadLen = Transmog::k_colorTokenRegistrarCallAobHeadLen;

            // Offset of the `lea rdx, [rip+name]` inside a matched head. That lea is always the LAST instruction of
            // the head and is always 7 bytes (opcode 48 8D 15 + disp32), so DERIVING it from the head length is what
            // keeps the two in step. Do not restate it as an independent constant: a head length that changes without
            // this offset following it decodes the name from the wrong address on every hit, every candidate then
            // fails the allow-list, and discovery reports zero slots with no error anywhere.
            constexpr std::size_t k_nameLeaOffset = k_patternHeadLen - 7;

            std::size_t totalHits = 0;
            std::size_t totalIter = 0;
            std::size_t accepted = 0;
            bool budgetExceeded = false;
            for (std::size_t pi = 0; pi < patterns.size() && !budgetExceeded; ++pi)
            {
                const auto &compiled = patterns[pi];

                // unchecked::find_pattern, not scan(): this sweep takes thousands of hits per pattern, and scan()
                // walks the OS page map on every call, which is a startup-time cost its own note warns against
                // paying in a loop. The raw twin does no page filtering, so the caller owns readability - and the
                // scope here IS one mapped PE image, every byte of it committed. Page gating also buys nothing at
                // this site: the query bytes live in this DLL's own .rdata, outside the scanned image, so a match
                // can never be the query finding itself.
                const auto *cursor = range.base.ptr<const std::byte>();
                const auto *const scanEnd = range.end().ptr<const std::byte>();
                std::size_t patternHits = 0;
                std::size_t iter = 0;
                for (; iter < k_maxItersPerPattern; ++iter)
                {
                    if (g_stopping.load(std::memory_order_acquire))
                    {
                        // Teardown asked for the module back. Abandoning mid-sweep is safe: every accepted slot is
                        // already in g_slots and the dedup makes a later scan idempotent.
                        budgetExceeded = true;
                        break;
                    }
                    if (clock::now() - t0 > k_timeBudget)
                    {
                        DMK::log().warning(
                            "[token-discovery] time budget exceeded at pattern={} iter={} hits={} (bailing)",
                            pi,
                            iter,
                            patternHits
                        );
                        budgetExceeded = true;
                        break;
                    }
                    const auto bytesLeft = static_cast<std::size_t>(scanEnd - cursor);
                    if (bytesLeft < k_patternHeadLen)
                        break;
                    const auto *const hit =
                        DMK::scan::unchecked::find_pattern(DMK::Region{DMK::Address{cursor}, bytesLeft}, compiled);
                    if (hit == nullptr)
                        break;
                    ++patternHits;

                    const auto matchAddr = reinterpret_cast<std::uintptr_t>(hit);

                    // Decode the `lea rdx, [rip+disp32]` target that closes the matched head. The disp32 sits 3
                    // bytes into the 7-byte lea, so the target resolves against the byte after the instruction.
                    const auto nameTarget = DMK::scan::resolve_rip_relative(DMK::Address{hit + k_nameLeaOffset}, 3, 7);
                    if (!nameTarget)
                    {
                        cursor = hit + 1;
                        continue;
                    }
                    const auto strAddr = nameTarget->raw();

                    // Diagnostic trace: log first 5 match decodes across all patterns (with a peek at the first 32
                    // bytes of the candidate name string) so we can sanity-check the decode pipeline.
                    static std::atomic<std::size_t> s_traceLeft{5};
                    if (s_traceLeft.load(std::memory_order_acquire) > 0)
                    {
                        if (s_traceLeft.fetch_sub(1, std::memory_order_acq_rel) > 0)
                        {
                            char preview[33] = {0};
                            peek_name_preview(strAddr, preview, sizeof(preview));
                            DMK::log().trace(
                                "[token-discovery] trace: P{} site=0x{:X} strAddr=0x{:X} preview='{}'",
                                pi + 1,
                                matchAddr,
                                strAddr,
                                preview
                            );
                        }
                    }

                    // Match against known names and proceed only on a hit. One guarded copy of the candidate feeds
                    // all 18 comparisons, and the names are short so the linear scan is cheap.
                    char candidate[k_name_window] = {0};
                    if (!read_foreign_name(strAddr, candidate))
                    {
                        cursor = hit + 1;
                        continue;
                    }
                    const KnownProp *matched_kp = nullptr;
                    for (const auto &kp : k_known)
                    {
                        if (std::strcmp(candidate, kp.name) == 0)
                        {
                            matched_kp = &kp;
                            break;
                        }
                    }
                    if (matched_kp != nullptr)
                    {
                        const auto slotAddr = decode_zero_write_slot(hit);
                        if (slotAddr != 0)
                        {
                            bool dup = false;
                            {
                                // The scan for a duplicate and the append have to be one critical section: released
                                // between them, a second scan could append the same slot twice.
                                std::lock_guard<std::mutex> lk(g_slotsMtx);
                                for (const auto &s : g_slots)
                                {
                                    if (s.slot_addr == slotAddr)
                                    {
                                        dup = true;
                                        break;
                                    }
                                }
                                if (!dup)
                                {
                                    g_slots.push_back({
                                        slotAddr,
                                        matched_kp->name,
                                        matched_kp->layer,
                                        matched_kp->channel,
                                    });
                                }
                            }
                            if (!dup)
                            {
                                ++accepted;
                                DMK::log().trace(
                                    "[token-discovery] slot=0x{:X} '{}' layer={} channel={} site=0x{:X} via P{}",
                                    slotAddr,
                                    matched_kp->name,
                                    matched_kp->layer,
                                    matched_kp->channel,
                                    matchAddr,
                                    pi + 1
                                );
                            }
                        }
                    }

                    cursor = hit + k_patternHeadLen;
                }
                // The cap exit is the one loop exit that used to say nothing. Silence here is the worst case of
                // the three: a truncated P1 (the superset pattern) drops the slot count below the retry baseline, so
                // retry_if_underpopulated never settles and re-scans on the hot path for the rest of the session,
                // with no line in the log pointing at the cause.
                if (iter >= k_maxItersPerPattern)
                {
                    DMK::log().warning(
                        "[token-discovery] pattern={} hit the {} iteration cap with {} hit(s) - its sweep was "
                        "TRUNCATED and later slots were not seen; raise k_maxItersPerPattern",
                        pi,
                        k_maxItersPerPattern,
                        patternHits
                    );
                }

                totalHits += patternHits;
                totalIter += iter;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
            DMK::log().info(
                "[token-discovery] scan complete: patterns={} iter={} hits={} slots={} elapsed_ms={}",
                compiledCount,
                totalIter,
                totalHits,
                accepted,
                static_cast<long long>(elapsed)
            );
            g_complete.store(true, std::memory_order_release);
        }
    } // namespace

    void run()
    {
        std::call_once(g_runOnce, []() { do_run(); });
    }

    bool is_complete() noexcept
    {
        return g_complete.load(std::memory_order_acquire);
    }

    // Re-run the AOB scan when the live capture looks underpopulated. Cold start misses registrar call sites whose
    // code pages have not been committed yet (Windows lazy commit). The engine also lazy-loads materials, so
    // registrars that live in material-specific shader-glue routines land in memory long after the initial init pass.
    //
    // No hard attempt cap. Mirroring the item_name_table catalog stability check: keep re-scanning on the hot path
    // until two consecutive scans produce the same slot count. THAT means the engine stopped adding new registrars
    // and the table settled. After settle, this becomes a permanent no-op for the rest of the session.
    //
    // Throttled to ~1.5 s between attempts so the 600-800 ms scan never runs in a hot loop. do_run() dedups
    // against g_slots, so an interrupted re-scan is safe.
    namespace
    {
        /// One re-scan, on the worker thread. Clearing @ref g_rescanBusy is deliberately its last act.
        void rescan_worker(std::stop_token stop, std::size_t expectedMin) noexcept
        {
            if (stop.stop_requested())
            {
                g_rescanBusy.store(false, std::memory_order_release);
                return;
            }
            const auto before = slot_count();
            // do_run sets g_complete=true at the end; allow re-run by resetting the flag locally (g_runOnce stays
            // satisfied for the original call path so external `run()` callers still skip).
            g_complete.store(false, std::memory_order_release);
            do_run();
            const auto after = slot_count();
            const auto prev = g_lastCount.exchange(after, std::memory_order_acq_rel);
            if (after != before)
            {
                DMK::log().info(
                    "[token-discovery] re-scan: slots {} -> {} (prev_total={} expectedMin={})",
                    before,
                    after,
                    prev,
                    expectedMin
                );
            }
            // Settle = two consecutive scans returned the same count AND the expected baseline is met. Without the
            // baseline gate, an early scan that finds 0 slots settles immediately on the next 0-slot scan and freezes
            // the retry permanently. A sweep abandoned by teardown must not settle on its short count.
            if (after == prev && after >= expectedMin && !g_stopping.load(std::memory_order_acquire))
            {
                g_settled.store(true, std::memory_order_release);
                DMK::log().info("[token-discovery] settled at {} slots; no further re-scans this session", after);
            }
            g_rescanBusy.store(false, std::memory_order_release);
        }
    } // namespace

    void retry_if_underpopulated(std::size_t expectedMin) noexcept
    {
        static std::atomic<std::int64_t> s_last_ms{0};

        if (g_settled.load(std::memory_order_acquire) || g_stopping.load(std::memory_order_acquire))
            return;
        // One clock owner for the whole module, so the apply window, the reinit deadlines and this throttle share a
        // time base by construction.
        const auto now = State::now_ms();
        const auto last = s_last_ms.load(std::memory_order_acquire);
        if (last != 0 && (now - last) < 1500)
            return;
        // Single-flight guard: only one worker runs at a time. Other concurrent callers see the flag and bail.
        bool expected = false;
        if (!g_rescanBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;
        s_last_ms.store(now, std::memory_order_release);

        // Dispatch rather than run. The caller is a detour on the engine's property setter, so the game thread must
        // leave here in microseconds; the sweep itself is half a second of work. Nothing downstream needs the result
        // this frame - an unclassified token still substitutes, it is only bucketed under "misc" in the UI until a
        // later scan names it.
        try
        {
            std::lock_guard<std::mutex> lk(g_rescanThreadMtx);
            if (g_stopping.load(std::memory_order_acquire))
            {
                g_rescanBusy.store(false, std::memory_order_release);
                return;
            }
            // The previous worker cleared g_rescanBusy as its last act, so it has finished; destroying it here
            // only collects the exited thread and cannot wait on a sweep.
            g_rescanWorker.reset();
            g_rescanWorker.emplace(
                "LtTokenRescan",
                [expectedMin](std::stop_token stop) { rescan_worker(stop, expectedMin); }
            );
        }
        catch (const std::exception &)
        {
            // Thread creation failed (resource exhaustion). Release the guard so a later call can retry; the feature
            // degrades to whatever slots are already known.
            g_rescanBusy.store(false, std::memory_order_release);
            DMK::log().warning("[token-discovery] could not start the re-scan worker; keeping the current slots");
        }
    }

    void stop_and_join_rescan() noexcept
    {
        g_stopping.store(true, std::memory_order_release);
        // The declaration is noexcept and std::mutex::lock throws std::system_error, so contain it here. The stop flag
        // is already set, so a failed join still leaves the worker on its way out.
        try
        {
            std::lock_guard<std::mutex> lk(g_rescanThreadMtx);
            // ~StoppableWorker requests stop and joins, so the reset IS the join.
            g_rescanWorker.reset();
        }
        catch (...)
        {
        }
    }

    int classify_layer(std::uint32_t tok) noexcept
    {
        if (!is_complete() || tok == 0)
            return -1;
        const auto t = tok & 0xFFFFu;
        std::lock_guard<std::mutex> lk(g_slotsMtx);
        for (const auto &s : g_slots)
        {
            const auto live = DMK::memory::read<std::uint32_t>(DMK::Address{s.slot_addr}).value_or(0) & 0xFFFFu;
            if (live != 0u && live == t)
                return s.layer;
        }
        return -1;
    }

    int classify_channel(std::uint32_t tok) noexcept
    {
        if (!is_complete() || tok == 0)
            return -1;
        const auto t = tok & 0xFFFFu;
        std::lock_guard<std::mutex> lk(g_slotsMtx);
        for (const auto &s : g_slots)
        {
            const auto live = DMK::memory::read<std::uint32_t>(DMK::Address{s.slot_addr}).value_or(0) & 0xFFFFu;
            if (live != 0u && live == t)
                return s.channel;
        }
        return -1;
    }

    std::uint32_t lookup_token_for_name(const char *name) noexcept
    {
        if (name == nullptr || !is_complete())
            return 0;
        std::lock_guard<std::mutex> lk(g_slotsMtx);
        for (const auto &s : g_slots)
        {
            if (std::strcmp(s.name, name) == 0)
            {
                const auto v = DMK::memory::read<std::uint32_t>(DMK::Address{s.slot_addr}).value_or(0);
                if (v != 0u && v != 0xFFFFFFFFu)
                    return v;
            }
        }
        return 0;
    }

    const char *name_for_token(std::uint32_t tok) noexcept
    {
        if (!is_complete() || tok == 0)
            return nullptr;
        const auto t = tok & 0xFFFFu;
        std::lock_guard<std::mutex> lk(g_slotsMtx);
        for (const auto &s : g_slots)
        {
            const auto live = DMK::memory::read<std::uint32_t>(DMK::Address{s.slot_addr}).value_or(0) & 0xFFFFu;
            if (live != 0u && live == t)
                return s.name;
        }
        return nullptr;
    }

    std::size_t slot_count() noexcept
    {
        std::lock_guard<std::mutex> lk(g_slotsMtx);
        return g_slots.size();
    }
} // namespace Transmog::ColorOverride::TokenSlotDiscovery
