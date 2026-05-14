#include "color_token_discovery.hpp"

#include <DetourModKit.hpp>
#include <DetourModKit/scanner.hpp>

#include <Windows.h>
#include <Psapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace DMK = DetourModKit;

namespace Transmog::ColorOverride::TokenSlotDiscovery
{
    namespace
    {
        struct KnownProp
        {
            const char *name;
            int layer;
            int channel;
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
            std::uintptr_t slot_addr;
            const char *name;
            int layer;
            int channel;
        };
        std::vector<DiscoveredSlot> g_slots;
        std::atomic<bool> g_complete{false};
        std::once_flag g_runOnce;

        bool name_matches(std::uintptr_t addr,
                          const char *name) noexcept
        {
            __try
            {
                const char *src =
                    reinterpret_cast<const char *>(addr);
                std::size_t i = 0;
                while (true)
                {
                    const char c = src[i];
                    const char n = name[i];
                    if (c != n) return false;
                    if (n == '\0') return true;
                    ++i;
                    if (i > 128) return false;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        std::uint32_t read_slot_value(
            std::uintptr_t addr) noexcept
        {
            __try
            {
                return *reinterpret_cast<const std::uint32_t *>(addr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        // SEH-guarded helper to read a name-string preview from a
        // resolved descriptor address. `out` is a fixed-size buffer
        // filled with up-to-32 printable chars + NUL. Non-printable
        // bytes are replaced with '?'. Returns true on any read.
        bool peek_name_preview(std::uintptr_t addr,
                               char *out,
                               std::size_t out_cap) noexcept
        {
            if (out == nullptr || out_cap == 0) return false;
            out[0] = '\0';
            __try
            {
                const char *sp =
                    reinterpret_cast<const char *>(addr);
                std::size_t i = 0;
                const std::size_t limit =
                    (out_cap - 1 < 32) ? out_cap - 1 : 32;
                for (; i < limit; ++i)
                {
                    const char c = sp[i];
                    if (c == '\0') break;
                    const auto uc =
                        static_cast<unsigned char>(c);
                    out[i] = (uc >= 0x20 && uc <= 0x7E) ? c : '?';
                }
                out[i] = '\0';
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                std::snprintf(out, out_cap, "<fault>");
                return false;
            }
        }

        // Try to decode a `lea r,[rip+disp32]` instruction at `p`,
        // expecting opcode prefix `prefix[0..2]`. Returns the
        // resolved target or 0 on prefix mismatch.
        std::uintptr_t decode_lea_rip(const std::byte *p,
                                      std::uint8_t b0,
                                      std::uint8_t b1,
                                      std::uint8_t b2) noexcept
        {
            __try
            {
                if (static_cast<std::uint8_t>(p[0]) != b0 ||
                    static_cast<std::uint8_t>(p[1]) != b1 ||
                    static_cast<std::uint8_t>(p[2]) != b2)
                    return 0;
                const std::int32_t disp =
                    *reinterpret_cast<const std::int32_t *>(p + 3);
                return reinterpret_cast<std::uintptr_t>(p + 7) +
                       static_cast<std::intptr_t>(disp);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        void do_run()
        {
            using clock = std::chrono::steady_clock;
            const auto t0 = clock::now();

            // Get module base + image size.
            const auto hmod = GetModuleHandleW(nullptr);
            if (hmod == nullptr)
            {
                DMK::Logger::get_instance().warning(
                    "[token-discovery] GetModuleHandle failed");
                g_complete.store(true, std::memory_order_release);
                return;
            }
            MODULEINFO mi{};
            if (!GetModuleInformation(GetCurrentProcess(), hmod,
                                      &mi, sizeof(mi)))
            {
                DMK::Logger::get_instance().warning(
                    "[token-discovery] GetModuleInformation failed");
                g_complete.store(true, std::memory_order_release);
                return;
            }
            const auto *base =
                reinterpret_cast<const std::byte *>(mi.lpBaseOfDll);
            const auto size = mi.SizeOfImage;

            // Registrar pattern (RE'd via IDA on sub_14274A3C0 and
            // sub_142749F10, the two registrar functions for dye-
            // property tokens):
            //
            //   <zero-write>               6 or 7 bytes, decoded
            //                              backward to get slot addr
            //   41 B9 FF FF 02 00          mov  r9d, 0x2FFFF  (6B)
            //   ?? 8D ?? 01                lea  r8d, [reg+1]  (4B)
            //                              REX/ModR/M wildcarded -
            //                              r13 (sub_14274A3C0) or
            //                              rdi (sub_142749F10).
            //   48 8D 15 ?? ?? ?? ??       lea  rdx, [name]   (7B)
            //   <rcx load>                 3 or 7 bytes (lea or mov)
            //   E8 ?? ?? ?? ??             call interner      (5B)
            //
            // We anchor on the 17-byte run from `mov r9d, 0x2FFFF`
            // through `lea rdx, [name]`. The `lea rdx` target is
            // the property name string. The slot address is decoded
            // from the zero-write instruction immediately PRECEDING
            // the `mov r9d`:
            //
            //   `89 3D disp32`       (6 bytes) - mov [rip+d], edi
            //   `44 89 2D disp32`    (7 bytes) - mov [rip+d], r13d
            //
            // Anchoring on the zero-write instead of `lea rcx` lets
            // us catch the first-entry-per-function case where rcx
            // is loaded via `mov rcx, rbx`/`mov rcx, rsi` from a
            // preloaded table-base register. That entry is always
            // `_dyeingColorMaskR` and matters because the live game
            // emits its token id (0x2FCE).
            constexpr std::string_view aob =
                "41 B9 FF FF 02 00 "
                "?? 8D ?? 01 "
                "48 8D 15 ?? ?? ?? ??";
            auto compiled = DMK::Scanner::parse_aob(aob);
            if (!compiled)
            {
                DMK::Logger::get_instance().warning(
                    "[token-discovery] AOB compile failed");
                g_complete.store(true, std::memory_order_release);
                return;
            }

            // SEH-guarded zero-write decode. The instruction
            // immediately preceding the matched `mov r9d, 0x2FFFF`
            // anchor is one of:
            //   `89 3D disp32`       (6B) - mov [rip+d], edi
            //   `44 89 2D disp32`    (7B) - mov [rip+d], r13d
            // Both encode the table slot address as RIP-relative
            // disp32 (resolved against the next-instruction RIP,
            // which is the anchor's start).
            auto decode_zero_write_slot =
                [](const std::byte *anchor) -> std::uintptr_t
            {
                __try
                {
                    // 6-byte form: `89 3D` at anchor-6
                    const auto b6_0 =
                        static_cast<std::uint8_t>(anchor[-6]);
                    const auto b6_1 =
                        static_cast<std::uint8_t>(anchor[-5]);
                    if (b6_0 == 0x89 && b6_1 == 0x3D)
                    {
                        const std::int32_t disp =
                            *reinterpret_cast<const std::int32_t *>(
                                anchor - 4);
                        return reinterpret_cast<std::uintptr_t>(
                                   anchor) +
                               static_cast<std::intptr_t>(disp);
                    }
                    // 7-byte form: `44 89 2D` at anchor-7
                    const auto b7_0 =
                        static_cast<std::uint8_t>(anchor[-7]);
                    const auto b7_1 =
                        static_cast<std::uint8_t>(anchor[-6]);
                    const auto b7_2 =
                        static_cast<std::uint8_t>(anchor[-5]);
                    if (b7_0 == 0x44 && b7_1 == 0x89 && b7_2 == 0x2D)
                    {
                        const std::int32_t disp =
                            *reinterpret_cast<const std::int32_t *>(
                                anchor - 4);
                        return reinterpret_cast<std::uintptr_t>(
                                   anchor) +
                               static_cast<std::intptr_t>(disp);
                    }
                    return 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    return 0;
                }
            };

            // Linear walk through the module: find_pattern returns
            // first match in [cursor..end]; advance past it and
            // continue.
            //
            // The 17-byte pattern can match a large number of false
            // positives early in the binary because the wildcard
            // bytes give it some slack. Real registrar sites cluster
            // around 0x142749F00 / 0x14274A3C0, so we may need to
            // walk past several thousand false-positive matches
            // before reaching them. Cap is set generously and the
            // wall-clock budget remains the real safety net.
            constexpr std::size_t k_maxIters = 50000;
            constexpr auto k_timeBudget =
                std::chrono::milliseconds(3000);
            // The matched pattern is 17 bytes. The anchor we care
            // about is the start of `41 B9 FF FF 02 00`. Advance
            // cursor past the matched range each iteration.
            constexpr std::size_t k_patternLen = 17;
            const std::byte *cursor = base;
            std::size_t bytes_left = size;
            std::size_t hits = 0;
            std::size_t accepted = 0;
            std::size_t iter = 0;
            for (; iter < k_maxIters; ++iter)
            {
                if (clock::now() - t0 > k_timeBudget)
                {
                    DMK::Logger::get_instance().warning(
                        "[token-discovery] time budget exceeded at "
                        "iter={} hits={} (bailing)",
                        iter, hits);
                    break;
                }
                if (bytes_left < k_patternLen) break;
                const auto *m = DMK::Scanner::find_pattern(
                    cursor, bytes_left, *compiled);
                if (m == nullptr) break;
                ++hits;

                const auto matchAddr =
                    reinterpret_cast<std::uintptr_t>(m);

                // Decode lea rdx target (at offset 10 in the
                // matched run; the disp32 is 3 bytes into the lea).
                const auto strAddr = decode_lea_rip(
                    m + 10, 0x48, 0x8D, 0x15);
                if (strAddr == 0)
                {
                    cursor = m + 1;
                    bytes_left = (base + size) - cursor;
                    continue;
                }

                // Diagnostic trace: log first 5 match decodes (with
                // a peek at the first 32 bytes of the candidate
                // name string) so we can sanity-check the decode
                // pipeline.
                static std::atomic<std::size_t> s_traceLeft{5};
                if (s_traceLeft.load(std::memory_order_acquire) > 0)
                {
                    if (s_traceLeft.fetch_sub(
                            1, std::memory_order_acq_rel) > 0)
                    {
                        char preview[33] = {0};
                        peek_name_preview(strAddr, preview,
                                          sizeof(preview));
                        DMK::Logger::get_instance().trace(
                            "[token-discovery] trace: site=0x{:X} "
                            "strAddr=0x{:X} preview='{}'",
                            matchAddr, strAddr, preview);
                    }
                }

                // Match against known names; only proceed if a hit.
                const KnownProp *matched_kp = nullptr;
                for (const auto &kp : k_known)
                {
                    if (name_matches(strAddr, kp.name))
                    {
                        matched_kp = &kp;
                        break;
                    }
                }
                if (matched_kp != nullptr)
                {
                    const auto slotAddr = decode_zero_write_slot(m);
                    if (slotAddr != 0)
                    {
                        bool dup = false;
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
                            g_slots.push_back(
                                {slotAddr, matched_kp->name,
                                 matched_kp->layer,
                                 matched_kp->channel});
                            ++accepted;
                            DMK::Logger::get_instance().info(
                                "[token-discovery] slot=0x{:X} '{}' "
                                "layer={} channel={} site=0x{:X}",
                                slotAddr, matched_kp->name,
                                matched_kp->layer,
                                matched_kp->channel, matchAddr);
                        }
                    }
                }

                cursor = m + k_patternLen;
                bytes_left = (base + size) - cursor;
            }

            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    clock::now() - t0).count();
            DMK::Logger::get_instance().info(
                "[token-discovery] scan complete: iter={} hits={} "
                "slots={} elapsed_ms={}",
                iter, hits, accepted,
                static_cast<long long>(elapsed));
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

    int classify_layer(std::uint32_t tok) noexcept
    {
        if (!is_complete() || tok == 0) return -1;
        const auto t = tok & 0xFFFFu;
        for (const auto &s : g_slots)
        {
            const auto live = read_slot_value(s.slot_addr) & 0xFFFFu;
            if (live != 0u && live == t) return s.layer;
        }
        return -1;
    }

    int classify_channel(std::uint32_t tok) noexcept
    {
        if (!is_complete() || tok == 0) return -1;
        const auto t = tok & 0xFFFFu;
        for (const auto &s : g_slots)
        {
            const auto live = read_slot_value(s.slot_addr) & 0xFFFFu;
            if (live != 0u && live == t) return s.channel;
        }
        return -1;
    }

    std::uint32_t lookup_token_for_name(
        const char *name) noexcept
    {
        if (name == nullptr || !is_complete()) return 0;
        for (const auto &s : g_slots)
        {
            if (std::strcmp(s.name, name) == 0)
            {
                const auto v = read_slot_value(s.slot_addr);
                if (v != 0u && v != 0xFFFFFFFFu) return v;
            }
        }
        return 0;
    }

    const char *name_for_token(std::uint32_t tok) noexcept
    {
        if (!is_complete() || tok == 0) return nullptr;
        const auto t = tok & 0xFFFFu;
        for (const auto &s : g_slots)
        {
            const auto live = read_slot_value(s.slot_addr) & 0xFFFFu;
            if (live != 0u && live == t) return s.name;
        }
        return nullptr;
    }

    std::size_t slot_count() noexcept
    {
        return g_slots.size();
    }
}
