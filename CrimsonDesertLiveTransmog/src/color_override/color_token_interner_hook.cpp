#include "color_token_interner_hook.hpp"

#include <DetourModKit.hpp>

#include <Windows.h>
#include <Psapi.h>

#include <array>
#include <atomic>
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

        // SEH-guarded reads.
        std::uint64_t safe_read_qword(std::uintptr_t addr) noexcept
        {
            __try
            {
                return *reinterpret_cast<const std::uint64_t *>(addr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }
        std::uint32_t safe_read_dword(std::uintptr_t addr) noexcept
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

        // SEH-guarded: scan up to 16 bytes after match for the `E8`
        // call opcode and decode its rel32 target. Returns 0 on
        // miss or fault.
        std::uintptr_t decode_call_after(
            const std::byte *match_end) noexcept
        {
            __try
            {
                for (std::size_t i = 0; i < 16; ++i)
                {
                    if (static_cast<std::uint8_t>(match_end[i]) ==
                        0xE8)
                    {
                        const std::int32_t disp =
                            *reinterpret_cast<const std::int32_t *>(
                                match_end + i + 1);
                        return reinterpret_cast<std::uintptr_t>(
                                   match_end + i + 5) +
                               static_cast<std::intptr_t>(disp);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return 0;
        }

        // Find sub_140F46680 (the interner) by anchoring on a
        // registrar call site we already know how to identify.
        // The 17-byte pattern `mov r9d, 0x2FFFF; lea r8d, [reg+1];
        // lea rdx, [name]` is followed by `lea rcx, [slot]` (7B) OR
        // `mov rcx, reg` (3B), then `E8 disp32` (call). The first
        // E8 opcode in the following 16 bytes is the call to the
        // interner.
        //
        // More reliable than AOB-scanning for the interner's
        // prologue (which collides with many other functions
        // sharing the same Microsoft __fastcall prologue -- we saw
        // a false-match at sub_1407959E0 in v1.06).
        std::uintptr_t find_interner_addr(
            const std::byte *base, std::size_t size) noexcept
        {
            constexpr std::string_view aob =
                "41 B9 FF FF 02 00 "
                "?? 8D ?? 01 "
                "48 8D 15 ?? ?? ?? ??";
            auto compiled = DMK::Scanner::parse_aob(aob);
            if (!compiled) return 0;
            const auto *m = DMK::Scanner::find_pattern(
                base, size, *compiled);
            if (m == nullptr) return 0;
            return decode_call_after(m + 17);
        }

        // Scan the function body for the first `48 89 05 disp32`
        // (mov [rip+disp32], rax) whose target lies inside the
        // loaded module. That's the write `qword_145E15620 = v10`
        // -- the static slot that holds the interner state pointer.
        // 0x312 is the offset in v1.06; we widen the search window
        // to ±0x200 around that to absorb compiler shifts in future
        // patches.
        // Scan for any `mov [rip+disp32], REG` writing to a module-
        // data slot whose runtime value is a heap pointer (the
        // interner's state). REX prefix is 0x48 (rax-r7) or 0x4C
        // (r8-r15). ModR/M byte uses RIP-relative addressing when
        // `(byte & 0xC7) == 0x05` (mod=00, r/m=101); the reg field
        // (bits 3-5) can be any register, hence the mask.
        //
        // Walks through the prologue + body looking for the FIRST
        // such write whose target slot contains a valid heap
        // pointer. In v1.06 the write is `mov [rip+disp32], rsi`
        // (`48 89 35 disp32`) at function offset 0x312.
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
        // (name, token) pair. Returns number captured.
        std::size_t walk_entries_array(
            std::uintptr_t entriesBase) noexcept
        {
            constexpr std::size_t k_maxWalk = 0x20000; // 128k entries
            constexpr std::ptrdiff_t k_entryStride = 32;
            constexpr std::ptrdiff_t k_offName = 0x08;
            constexpr std::ptrdiff_t k_offToken = 0x18;
            std::size_t captured = 0;
            std::size_t consecutiveBad = 0;
            for (std::size_t i = 0; i < k_maxWalk; ++i)
            {
                const auto entryAddr =
                    entriesBase + i * k_entryStride;
                const auto namePtr = safe_read_qword(
                    entryAddr + k_offName);
                const auto token = safe_read_dword(
                    entryAddr + k_offToken);
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
            return captured;
        }

        void do_init() noexcept
        {
            auto &logger = DMK::Logger::get_instance();
            const auto hmod = GetModuleHandleW(nullptr);
            if (hmod == nullptr) return;
            MODULEINFO mi{};
            if (!GetModuleInformation(GetCurrentProcess(), hmod,
                                      &mi, sizeof(mi)))
                return;
            const auto modBase =
                reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
            const auto modSize = mi.SizeOfImage;

            const auto funcAddr = find_interner_addr(
                reinterpret_cast<const std::byte *>(mi.lpBaseOfDll),
                modSize);
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
            const auto stateAddr = safe_read_qword(stateSlot);
            if (stateAddr < 0x10000ULL)
            {
                logger.warning(
                    "[interner-hook] state pointer at 0x{:X} not "
                    "yet initialised (will retry lazily)",
                    stateSlot);
                return;
            }
            // Per RE (decompile of sub_140F46680):
            //   state + 0x30 = num_buckets       (u32)
            //   state + 0x40 = bucket_array_ptr  (u64)
            //   state + 0x48 = entries_array_ptr (u64)
            // Reading +0x50 by mistake aliases the sentinel-cap
            // field (`0x2FFFF...`) and produces a garbage entries
            // pointer that captures 0 names.
            const auto entriesBase = safe_read_qword(stateAddr + 0x48);
            if (entriesBase < 0x10000ULL)
            {
                logger.warning(
                    "[interner-hook] entries-array pointer at "
                    "state+0x50 not initialised");
                return;
            }
            const auto captured = walk_entries_array(entriesBase);
            logger.info(
                "[interner-hook] func=0x{:X} stateSlot=0x{:X} "
                "state=0x{:X} entries=0x{:X} captured={}",
                funcAddr, stateSlot, stateAddr, entriesBase,
                captured);
            g_dumped.store(true, std::memory_order_release);
        }
    } // namespace

    bool init() noexcept
    {
        std::call_once(g_initOnce, []() { do_init(); });
        return g_dumped.load(std::memory_order_acquire);
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
